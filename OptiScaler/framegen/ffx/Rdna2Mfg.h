#pragma once
// Rdna2Mfg.h - multi-frame generation (3x..6x) for the RDNA2 DLSS-FG provider on FSR-FG's swapchain. Header only, no
// OptiScaler dependencies (the offline harness E:\dlss-runtime\fgport\mfg builds it as is).
//
// FFX's swapchain presents exactly one generated frame per real frame (numGeneratedFrames is fixed at 1). For N > 1:
//  - frameGenerationCallback: Acquire() hands out N-1 textures of our own; Rdna2FgRecordOutput writes frame 1 into FFX's
//    output and frames 2..N into them (t = i / (N+1)); Commit() records how many it wrote.
//  - FFX's presentCallback (ours while N > 1) copies FFX's frame into the back buffer and calls Tag() on FFX's presenter
//    thread, right before that thread presents through OptiScaler's wrapped real swapchain.
//  - WrappedIDXGISwapChain4::Present -> Present(): after FFX's generated frame it presents frames 2..N itself, each
//    copied into the next back buffer on the swapchain's queue. Every present of a real frame's burst is spaced by
//    measured real-frame interval / (N+1) (QPC); FFX's own pacing is switched off (safety margin) so it never delays them.
// Present flags / sync interval / AntiLag 2 frame type are FFX's, as set for its generated frame.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

namespace Rdna2Mfg
{
constexpr UINT kMaxFrames = 5; // 6x
constexpr UINT kSets = 2;      // extra-frame sets in flight (FFX double-buffers its own output the same way)
constexpr UINT kLists = 8;

struct Set
{
    ID3D12Resource* tex[kMaxFrames - 1] {};
    UINT64 frame = ~0ull;
    UINT count = 0;      // frames written by the runtime (Commit)
    bool pending = false; // handed out, not presented yet
    UINT64 freeAt = 0;    // copy fence value after its last present
};

struct Stats
{
    UINT64 bursts = 0, extras = 0, noSet = 0, noQueue = 0;
};

struct State
{
    std::mutex mu;
    std::atomic<UINT> frames { 1 }; // generated frames per real frame
    double slack = 0.98;             // spacing = slack * interval / (N+1): drains faster than frames arrive
    void (*log)(const char*) = nullptr;

    ID3D12Device* dev = nullptr;
    ID3D12Fence* fence = nullptr; // signalled on the present queue after each extra copy
    std::atomic<UINT64> fenceValue { 0 };
    D3D12_RESOURCE_DESC desc {};
    Set set[kSets];
    std::vector<std::pair<ID3D12Resource*, UINT64>> grave;

    // presenter thread only
    ID3D12Device* listDev = nullptr;
    ID3D12CommandAllocator* al[kLists] {};
    ID3D12GraphicsCommandList* cl[kLists] {};
    UINT64 alAt[kLists] {};
    UINT alNext = 0;
    HANDLE ev = nullptr;
    double periodMs = 0, lastArrivalMs = 0, lastPresentMs = 0, lastGenMs = 0;
    UINT64 entryFrame = ~0ull;
    UINT entryPresents = 0;
    UINT behindRun = 0; // consecutive real frames that arrived before their slot
    DWORD presenterThread = 0;
    Stats stats;
};

inline State& S()
{
    static State s;
    return s;
}

struct TagData
{
    bool valid = false, generated = false;
    UINT64 frame = 0;
};
inline thread_local TagData tag;

inline double NowMs()
{
    static const double f = []
    {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return (double) q.QuadPart / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart / f;
}

// High resolution timer a millisecond at a time, then a spin
inline void SleepUntil(double untilMs)
{
    static thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (double left = untilMs - NowMs(); left > 0; left = untilMs - NowMs())
    {
        LARGE_INTEGER due { .QuadPart = -(LONGLONG) ((std::min)(left - 0.5, 1.0) * 10000) };
        if (left > 0.6 && timer && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
            WaitForSingleObject(timer, 100);
        else
            YieldProcessor();
    }
}

inline void Log(const char* m)
{
    if (S().log)
        S().log(m);
}

// Called with mu held
inline void Sweep(State& s)
{
    if (!s.fence)
        return;
    const UINT64 done = s.fence->GetCompletedValue();
    std::erase_if(s.grave,
                  [done](auto& g)
                  {
                      if (g.second > done)
                          return false;
                      g.first->Release();
                      return true;
                  });
}

inline void Bury(State& s)
{
    for (auto& st : s.set)
    {
        for (auto*& t : st.tex)
            if (t)
                s.grave.push_back({ t, st.freeAt }), t = nullptr;
        st = Set {};
    }
}

// Generated frames per real frame (1 = 2x .. kMaxFrames = 6x)
inline void SetFrames(UINT n) { S().frames = std::clamp(n, 1u, kMaxFrames); }
inline UINT Frames() { return S().frames; }

// frameGenerationCallback: textures for frames 2..want+1 of `frame`, shaped like FFX's output `like`. 0: none free, this
// frame generates FFX's one frame only.
inline UINT Acquire(ID3D12Resource* like, UINT64 frame, UINT want, ID3D12Resource** out)
{
    auto& s = S();
    want = (std::min)(want, kMaxFrames - 1);
    if (!like || want == 0)
        return 0;

    std::lock_guard lk(s.mu);
    ID3D12Device* dev = nullptr;
    if (FAILED(like->GetDevice(IID_PPV_ARGS(&dev))))
        return 0;
    dev->Release();

    if (s.dev != dev)
    {
        Bury(s);
        for (auto& g : s.grave) // a new device: the old one's fence can't tell, the old frames are long done
            g.first->Release();
        s.grave.clear();
        if (s.fence)
            s.fence->Release();
        s.fence = nullptr;
        s.dev = dev;
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence))))
            return 0;
        s.fenceValue = 0;
    }

    auto d = like->GetDesc();
    d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    d.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    if (d.Width != s.desc.Width || d.Height != s.desc.Height || d.Format != s.desc.Format)
    {
        Bury(s);
        s.desc = d;
    }

    Sweep(s);
    const UINT64 done = s.fence->GetCompletedValue();
    Set* pick = nullptr;
    for (auto& st : s.set)
    {
        if (st.pending && frame - st.frame > 8) // FFX dropped that frame's presents: its writes were on this queue
            st.pending = false;
        if (!pick && !st.pending && st.freeAt <= done)
            pick = &st;
    }
    if (!pick)
    {
        s.stats.noSet++;
        return 0;
    }

    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
    for (UINT i = 0; i < want; i++)
    {
        if (!pick->tex[i] && FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &s.desc,
                                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                                 IID_PPV_ARGS(&pick->tex[i]))))
        {
            Log("RDNA2 MFG: extra frame texture allocation failed");
            return 0;
        }
        out[i] = pick->tex[i];
    }
    pick->pending = true;
    pick->frame = frame;
    pick->count = 0;
    return want;
}

// After Rdna2FgRecordOutput: `recorded` frames in total were written for `frame` (FFX's + ours)
inline void Commit(UINT64 frame, int recorded)
{
    auto& s = S();
    std::lock_guard lk(s.mu);
    for (auto& st : s.set)
        if (st.pending && st.frame == frame)
        {
            st.count = recorded > 1 ? (UINT) recorded - 1 : 0;
            st.pending = st.count > 0;
        }
}

// FFX's presentCallback, on the thread that presents next
inline void Tag(bool generated, UINT64 frame) { tag = { true, generated, frame }; }

inline void Release()
{
    auto& s = S();
    std::lock_guard lk(s.mu);
    Bury(s);
    if (s.fence && s.ev && s.fence->GetCompletedValue() < s.fenceValue &&
        SUCCEEDED(s.fence->SetEventOnCompletion(s.fenceValue, s.ev)))
        WaitForSingleObject(s.ev, 500);
    Sweep(s);
}

// Holds the present until `slots` spacings after the previous one
inline void Pace(State& s, UINT slots, UINT n)
{
    const double d = s.slack * s.periodMs / (n + 1);
    const double until = s.lastPresentMs + slots * d;
    const double now = NowMs();
    if (s.periodMs > 0 && s.lastPresentMs > 0 && until > now && until - now < 2 * s.periodMs)
        SleepUntil(until);
    s.lastPresentMs = NowMs();
}

// Copies `src` into the swapchain's current back buffer on `q`; false if it can't
inline bool CopyToBackBuffer(State& s, ID3D12CommandQueue* q, IDXGISwapChain* sc, ID3D12Resource* src)
{
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(sc->QueryInterface(IID_PPV_ARGS(&sc3))))
        return false;
    ID3D12Resource* bb = nullptr;
    const HRESULT hr = sc3->GetBuffer(sc3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
    sc3->Release();
    if (FAILED(hr))
        return false;

    ID3D12Device* dev = nullptr;
    q->GetDevice(IID_PPV_ARGS(&dev));
    if (dev)
        dev->Release();
    if (dev != s.dev) // the fence and the textures live on the device FFX's output came from
    {
        bb->Release();
        return false;
    }
    if (s.listDev != dev)
    {
        for (UINT i = 0; i < kLists; i++) // a new device: the old lists are idle (FFX drained its presents)
        {
            if (s.cl[i])
                s.cl[i]->Release();
            if (s.al[i])
                s.al[i]->Release();
            s.cl[i] = nullptr, s.al[i] = nullptr, s.alAt[i] = 0;
        }
        s.listDev = dev;
    }

    const UINT k = s.alNext++ % kLists;
    if (!s.al[k])
    {
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.al[k]))) ||
            FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.al[k], nullptr,
                                          IID_PPV_ARGS(&s.cl[k]))))
        {
            bb->Release();
            return false;
        }
        s.cl[k]->Close();
    }
    if (!s.ev)
        s.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (s.fence->GetCompletedValue() < s.alAt[k] &&
        (FAILED(s.fence->SetEventOnCompletion(s.alAt[k], s.ev)) || WaitForSingleObject(s.ev, 100) != WAIT_OBJECT_0))
    {
        bb->Release();
        return false;
    }

    auto* cl = s.cl[k];
    s.al[k]->Reset();
    cl->Reset(s.al[k], nullptr);
    D3D12_RESOURCE_BARRIER b[2] {};
    b[0].Transition = { src, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_STATE_COPY_SOURCE };
    b[1].Transition = { bb, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT,
                        D3D12_RESOURCE_STATE_COPY_DEST };
    cl->ResourceBarrier(2, b);
    cl->CopyResource(bb, src);
    std::swap(b[0].Transition.StateBefore, b[0].Transition.StateAfter);
    std::swap(b[1].Transition.StateBefore, b[1].Transition.StateAfter);
    cl->ResourceBarrier(2, b);
    cl->Close();
    ID3D12CommandList* l[] = { cl };
    q->ExecuteCommandLists(1, l);
    const UINT64 v = ++s.fenceValue;
    q->Signal(s.fence, v);
    s.alAt[k] = v;
    bb->Release();
    return true;
}

// The wrapped real swapchain's Present. `queueOwner`: what the swapchain was created with (its D3D12 queue).
// `present` presents the back buffer as it is.
inline HRESULT Present(IUnknown* queueOwner, IDXGISwapChain* sc, UINT flags, const std::function<HRESULT()>& present)
{
    auto& s = S();
    const TagData t = tag;
    tag.valid = false;
    const UINT n = s.frames;
    if (!t.valid || (flags & DXGI_PRESENT_TEST) || n <= 1 || !s.fence)
        return present();

    const double now = NowMs();
    if (t.generated)
    {
        s.presenterThread = GetCurrentThreadId();
        s.lastGenMs = now;
    }
    else if (GetCurrentThreadId() != s.presenterThread || now - s.lastGenMs > 250)
        return present(); // FG paused / off: FFX presents real frames only, unpaced

    if (t.frame != s.entryFrame) // a new real frame reached the presenter: its interval sets the spacing
    {
        // Arriving before the previous burst's spacing ran out means the game waited for us (FFX holds its next
        // Present until this thread composed the last real frame): the interval is ours, not the game's, shorten it
        const double iv = now - s.lastArrivalMs;
        const bool early = s.periodMs > 0 && now < s.lastPresentMs + s.slack * s.periodMs / (n + 1);
        s.behindRun = early ? s.behindRun + 1 : 0;
        const bool behind = s.behindRun >= 2; // one early frame is jitter, two in a row are us
        s.periodMs = s.lastArrivalMs == 0 || iv > 100 ? 0
                     : behind                         ? 0.98 * s.periodMs
                     : s.periodMs > 0                 ? 0.9 * s.periodMs + 0.1 * iv
                                                      : iv;
        s.lastArrivalMs = now;
        s.entryFrame = t.frame;
        s.entryPresents = 0;
    }

    // The real frame closes its N+1 slots, also when generated frames of the burst are missing
    Pace(s, t.generated ? 1 : n + 1 - (std::min)(s.entryPresents, n), n);
    HRESULT r = present();
    s.entryPresents++;
    if (!t.generated || FAILED(r))
        return r;

    // Frames 2..N
    ID3D12Resource* tex[kMaxFrames - 1] {};
    UINT count = 0;
    Set* st = nullptr;
    {
        std::lock_guard lk(s.mu);
        for (auto& x : s.set)
            if (x.pending && x.frame == t.frame && x.count > 0)
                st = &x;
        if (!st)
            return r;
        count = st->count;
        for (UINT i = 0; i < count; i++)
            (tex[i] = st->tex[i])->AddRef(); // a resize may bury the set meanwhile
    }

    ID3D12CommandQueue* q = nullptr;
    if (!queueOwner || FAILED(queueOwner->QueryInterface(IID_PPV_ARGS(&q))))
        s.stats.noQueue++;

    UINT64 last = 0;
    for (UINT i = 0; q && i < count && SUCCEEDED(r); i++)
    {
        if (!CopyToBackBuffer(s, q, sc, tex[i]))
            break;
        last = s.fenceValue;
        Pace(s, 1, n);
        r = present();
        s.entryPresents++;
        s.stats.extras++;
    }
    if (q)
        q->Release();
    s.stats.bursts++;

    std::lock_guard lk(s.mu);
    for (UINT i = 0; i < count; i++)
        s.grave.push_back({ tex[i], last }); // our reference, dropped once the copies are done
    if (st->pending && st->frame == t.frame)
    {
        st->freeAt = (std::max)(st->freeAt, last);
        st->pending = false;
    }
    return r;
}
} // namespace Rdna2Mfg
