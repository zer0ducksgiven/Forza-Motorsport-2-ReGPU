#pragma once
// A command list recorder: the renderer's D3D12 calls are written to a compact buffer and
// replayed into the real command list on a worker thread, so the command processor never
// waits on the driver (which costs ~15% of it per draw). Same method names and arguments
// as ID3D12GraphicsCommandList for the calls the renderer makes. Real() hands out the real
// list once everything recorded so far has been replayed (for code that records on it
// directly, and before Close/Execute). FM2_NO_RECORD_THREAD=1 calls the driver directly.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <d3d12.h>

class RecordedList
{
public:
    // direct: call the list right away (draw workers, which run in parallel themselves).
    void Init(ID3D12GraphicsCommandList* real, bool direct = false)
    {
        m_real = real;
        m_direct = direct || getenv("FM2_NO_RECORD_THREAD") != nullptr;
        if (!m_direct)
            std::thread([this] { Worker(); }).detach();
    }

    // Records into another list from now on (after replaying everything so far).
    void Rebind(ID3D12GraphicsCommandList* real)
    {
        Drain();
        m_real = real;
    }

    // The real list, with everything recorded so far replayed into it.
    ID3D12GraphicsCommandList* Real()
    {
        Drain();
        return m_real;
    }

    void ResourceBarrier(UINT n, const D3D12_RESOURCE_BARRIER* b) { Put(kBarrier, &n, sizeof(n), b, sizeof(*b) * n); }
    void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z, const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box)
    {
        struct A { D3D12_TEXTURE_COPY_LOCATION dst, src; UINT x, y, z; BOOL hasBox; D3D12_BOX box; } a{ *dst, *src, x, y, z, box != nullptr, box ? *box : D3D12_BOX{} };
        Put(kCopyTexture, &a, sizeof(a));
    }
    void SetGraphicsRootSignature(ID3D12RootSignature* r) { Put(kRootSignature, &r, sizeof(r)); }
    void SetComputeRootSignature(ID3D12RootSignature* r) { Put(kComputeRootSignature, &r, sizeof(r)); }
    void SetComputeRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) { struct A { UINT i; D3D12_GPU_DESCRIPTOR_HANDLE h; } a{ i, h }; Put(kComputeTable, &a, sizeof(a)); }
    void SetComputeRoot32BitConstants(UINT i, UINT n, const void* data, UINT offset)
    {
        struct A { UINT i, n, offset; } a{ i, n, offset };
        Put(kComputeRoot32, &a, sizeof(a), data, n * 4);
    }
    void Dispatch(UINT x, UINT y, UINT z) { UINT a[3] = { x, y, z }; Put(kDispatch, a, sizeof(a)); }
    void SetGraphicsRootDescriptorTable(UINT i, D3D12_GPU_DESCRIPTOR_HANDLE h) { struct A { UINT i; D3D12_GPU_DESCRIPTOR_HANDLE h; } a{ i, h }; Put(kTable, &a, sizeof(a)); }
    void SetGraphicsRootConstantBufferView(UINT i, D3D12_GPU_VIRTUAL_ADDRESS v) { struct A { UINT i; D3D12_GPU_VIRTUAL_ADDRESS v; } a{ i, v }; Put(kCbv, &a, sizeof(a)); }
    void SetGraphicsRoot32BitConstants(UINT i, UINT n, const void* data, UINT offset)
    {
        struct A { UINT i, n, offset; } a{ i, n, offset };
        Put(kRoot32, &a, sizeof(a), data, n * 4);
    }
    void OMSetRenderTargets(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs, BOOL single, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
    {
        // Only the forms the renderer uses: up to three render targets (separate handles: colour, motion and the
        // reactive mask), optional depth.
        struct A { UINT n; BOOL single, hasDsv; D3D12_CPU_DESCRIPTOR_HANDLE rtv[3], dsv; } a{ n, single, dsv != nullptr, {}, dsv ? *dsv : D3D12_CPU_DESCRIPTOR_HANDLE{} };
        for (UINT i = 0; i < n && i < 3; i++)
            a.rtv[i] = rtvs[i];
        Put(kTargets, &a, sizeof(a));
    }
    void SetPipelineState(ID3D12PipelineState* p) { Put(kPipeline, &p, sizeof(p)); }
    void RSSetViewports(UINT n, const D3D12_VIEWPORT* v) { Put(kViewports, &n, sizeof(n), v, sizeof(*v) * n); }
    void RSSetScissorRects(UINT n, const D3D12_RECT* r) { Put(kScissors, &n, sizeof(n), r, sizeof(*r) * n); }
    void OMSetStencilRef(UINT ref) { Put(kStencilRef, &ref, sizeof(ref)); }
    void OMSetBlendFactor(const FLOAT f[4]) { Put(kBlendFactor, f, sizeof(FLOAT) * 4); }
    void IASetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY t) { Put(kTopology, &t, sizeof(t)); }
    void SetDescriptorHeaps(UINT n, ID3D12DescriptorHeap* const* h) { Put(kHeaps, &n, sizeof(n), h, sizeof(*h) * n); }
    void DrawInstanced(UINT v, UINT i, UINT sv, UINT si) { UINT a[4] = { v, i, sv, si }; Put(kDraw, a, sizeof(a)); }
    void DrawIndexedInstanced(UINT n, UINT i, UINT si, INT bv, UINT sinst) { UINT a[5] = { n, i, si, UINT(bv), sinst }; Put(kDrawIndexed, a, sizeof(a)); }
    void IASetIndexBuffer(const D3D12_INDEX_BUFFER_VIEW* v) { Put(kIndexBuffer, v, sizeof(*v)); }
    void CopyBufferRegion(ID3D12Resource* dst, UINT64 dstOffset, ID3D12Resource* src, UINT64 srcOffset, UINT64 bytes)
    {
        struct A { ID3D12Resource* dst; UINT64 dstOffset; ID3D12Resource* src; UINT64 srcOffset, bytes; } a{ dst, dstOffset, src, srcOffset, bytes };
        Put(kCopyBuffer, &a, sizeof(a));
    }
    void BeginQuery(ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) { struct A { ID3D12QueryHeap* h; D3D12_QUERY_TYPE t; UINT i; } a{ h, t, i }; Put(kBeginQuery, &a, sizeof(a)); }
    void EndQuery(ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT i) { struct A { ID3D12QueryHeap* h; D3D12_QUERY_TYPE t; UINT i; } a{ h, t, i }; Put(kEndQuery, &a, sizeof(a)); }
    void ResolveQueryData(ID3D12QueryHeap* h, D3D12_QUERY_TYPE t, UINT start, UINT n, ID3D12Resource* dst, UINT64 offset)
    {
        struct A { ID3D12QueryHeap* h; D3D12_QUERY_TYPE t; UINT start, n; ID3D12Resource* dst; UINT64 offset; } a{ h, t, start, n, dst, offset };
        Put(kResolveQuery, &a, sizeof(a));
    }
    void ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT c[4], UINT nRects, const D3D12_RECT* rects)
    {
        struct A { D3D12_CPU_DESCRIPTOR_HANDLE rtv; FLOAT c[4]; UINT n; } a{ rtv, { c[0], c[1], c[2], c[3] }, nRects };
        Put(kClearRtv, &a, sizeof(a), rects, sizeof(D3D12_RECT) * nRects);
    }
    void ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE dsv, D3D12_CLEAR_FLAGS flags, FLOAT depth, UINT8 stencil, UINT nRects, const D3D12_RECT* rects)
    {
        struct A { D3D12_CPU_DESCRIPTOR_HANDLE dsv; D3D12_CLEAR_FLAGS flags; FLOAT depth; UINT8 stencil; UINT n; } a{ dsv, flags, depth, stencil, nRects };
        Put(kClearDsv, &a, sizeof(a), rects, sizeof(D3D12_RECT) * nRects);
    }
    // RTV and DSV descriptors are read when OMSetRenderTargets and the clears are recorded
    // into the real list, so writes to them are replayed in order too.
    void CreateRenderTargetView(ID3D12Device* d, ID3D12Resource* r, D3D12_CPU_DESCRIPTOR_HANDLE h)
    {
        struct A { ID3D12Device* d; ID3D12Resource* r; D3D12_CPU_DESCRIPTOR_HANDLE h; } a{ d, r, h };
        Put(kCreateRtv, &a, sizeof(a));
    }
    void CreateDepthStencilView(ID3D12Device* d, ID3D12Resource* r, const D3D12_DEPTH_STENCIL_VIEW_DESC& desc, D3D12_CPU_DESCRIPTOR_HANDLE h)
    {
        struct A { ID3D12Device* d; ID3D12Resource* r; D3D12_DEPTH_STENCIL_VIEW_DESC desc; D3D12_CPU_DESCRIPTOR_HANDLE h; } a{ d, r, desc, h };
        Put(kCreateDsv, &a, sizeof(a));
    }
    // Close and Reset go straight to the real list after a drain.
    HRESULT Close() { return Real()->Close(); }
    HRESULT Reset(ID3D12CommandAllocator* a, ID3D12PipelineState* p) { return Real()->Reset(a, p); }

private:
    enum Op : uint32_t
    {
        kBarrier, kCopyTexture, kRootSignature, kTable, kCbv, kRoot32, kTargets, kPipeline, kViewports, kScissors, kStencilRef, kBlendFactor,
        kTopology, kHeaps, kDraw, kEndQuery, kResolveQuery, kClearRtv, kClearDsv, kCreateRtv, kCreateDsv, kDrawIndexed, kIndexBuffer, kCopyBuffer,
        kComputeRootSignature, kComputeTable, kComputeRoot32, kDispatch, kBeginQuery
    };
    static constexpr size_t kChunk = 64 << 10;

    ID3D12GraphicsCommandList* m_real = nullptr;
    bool m_direct = false;
    std::vector<uint8_t> m_current;
    std::mutex m_mutex;
    std::condition_variable m_work, m_idle;
    std::deque<std::vector<uint8_t>> m_queue, m_spare;
    bool m_busy = false;

    void Put(Op op, const void* a, size_t na, const void* b = nullptr, size_t nb = 0)
    {
        const size_t at = m_current.size();
        const uint32_t header[2] = { op, uint32_t((na + nb + 7) & ~size_t(7)) }; // 8-byte aligned commands
        m_current.resize(at + sizeof(header) + header[1]);
        uint8_t* p = m_current.data() + at;
        memcpy(p, header, sizeof(header));
        memcpy(p + sizeof(header), a, na);
        if (nb)
            memcpy(p + sizeof(header) + na, b, nb);
        if (m_direct)
        {
            Replay(m_current.data(), m_current.size());
            m_current.clear();
        }
        else if (m_current.size() >= kChunk)
            Submit();
    }

    void Submit()
    {
        if (m_current.empty())
            return;
        std::lock_guard lock(m_mutex);
        m_queue.push_back(std::move(m_current));
        if (!m_spare.empty())
        {
            m_current = std::move(m_spare.back());
            m_spare.pop_back();
        }
        m_current.clear();
        m_current.reserve(kChunk + 4096);
        m_work.notify_one();
    }

    void Drain()
    {
        if (m_direct)
            return;
        Submit();
        std::unique_lock lock(m_mutex);
        m_idle.wait(lock, [this] { return m_queue.empty() && !m_busy; });
    }

    void Worker()
    {
        SetThreadDescription(GetCurrentThread(), L"command list replay");
        for (;;)
        {
            std::vector<uint8_t> chunk;
            {
                std::unique_lock lock(m_mutex);
                m_work.wait(lock, [this] { return !m_queue.empty(); });
                chunk = std::move(m_queue.front());
                m_queue.pop_front();
                m_busy = true;
            }
            Replay(chunk.data(), chunk.size());
            {
                std::lock_guard lock(m_mutex);
                chunk.clear();
                m_spare.push_back(std::move(chunk));
                m_busy = false;
                if (m_queue.empty())
                    m_idle.notify_all();
            }
        }
    }

    template <typename T>
    static const T& At(const uint8_t* p) { return *reinterpret_cast<const T*>(p); }

    void Replay(const uint8_t* p, size_t size)
    {
        ID3D12GraphicsCommandList* l = m_real;
        for (const uint8_t* end = p + size; p < end;)
        {
            uint32_t header[2];
            memcpy(header, p, sizeof(header));
            const uint8_t* a = p + sizeof(header);
            p = a + header[1];
            switch (header[0])
            {
            case kBarrier: l->ResourceBarrier(At<UINT>(a), reinterpret_cast<const D3D12_RESOURCE_BARRIER*>(a + sizeof(UINT))); break;
            case kCopyTexture:
            {
                struct A { D3D12_TEXTURE_COPY_LOCATION dst, src; UINT x, y, z; BOOL hasBox; D3D12_BOX box; };
                const A& c = At<A>(a);
                l->CopyTextureRegion(&c.dst, c.x, c.y, c.z, &c.src, c.hasBox ? &c.box : nullptr);
                break;
            }
            case kRootSignature: l->SetGraphicsRootSignature(At<ID3D12RootSignature*>(a)); break;
            case kTable:
            {
                struct A { UINT i; D3D12_GPU_DESCRIPTOR_HANDLE h; };
                l->SetGraphicsRootDescriptorTable(At<A>(a).i, At<A>(a).h);
                break;
            }
            case kCbv:
            {
                struct A { UINT i; D3D12_GPU_VIRTUAL_ADDRESS v; };
                l->SetGraphicsRootConstantBufferView(At<A>(a).i, At<A>(a).v);
                break;
            }
            case kRoot32:
            {
                struct A { UINT i, n, offset; };
                const A& c = At<A>(a);
                l->SetGraphicsRoot32BitConstants(c.i, c.n, a + sizeof(A), c.offset);
                break;
            }
            case kTargets:
            {
                struct A { UINT n; BOOL single, hasDsv; D3D12_CPU_DESCRIPTOR_HANDLE rtv[3], dsv; };
                const A& c = At<A>(a);
                l->OMSetRenderTargets(c.n, c.n ? c.rtv : nullptr, c.single, c.hasDsv ? &c.dsv : nullptr);
                break;
            }
            case kPipeline: l->SetPipelineState(At<ID3D12PipelineState*>(a)); break;
            case kViewports: l->RSSetViewports(At<UINT>(a), reinterpret_cast<const D3D12_VIEWPORT*>(a + sizeof(UINT))); break;
            case kScissors: l->RSSetScissorRects(At<UINT>(a), reinterpret_cast<const D3D12_RECT*>(a + sizeof(UINT))); break;
            case kStencilRef: l->OMSetStencilRef(At<UINT>(a)); break;
            case kBlendFactor: l->OMSetBlendFactor(reinterpret_cast<const FLOAT*>(a)); break;
            case kTopology: l->IASetPrimitiveTopology(At<D3D12_PRIMITIVE_TOPOLOGY>(a)); break;
            case kHeaps: l->SetDescriptorHeaps(At<UINT>(a), reinterpret_cast<ID3D12DescriptorHeap* const*>(a + sizeof(UINT))); break;
            case kDraw:
            {
                const UINT* d = reinterpret_cast<const UINT*>(a);
                l->DrawInstanced(d[0], d[1], d[2], d[3]);
                break;
            }
            case kBeginQuery:
            {
                struct A { ID3D12QueryHeap* h; D3D12_QUERY_TYPE t; UINT i; };
                l->BeginQuery(At<A>(a).h, At<A>(a).t, At<A>(a).i);
                break;
            }
            case kEndQuery:
            {
                struct A { ID3D12QueryHeap* h; D3D12_QUERY_TYPE t; UINT i; };
                l->EndQuery(At<A>(a).h, At<A>(a).t, At<A>(a).i);
                break;
            }
            case kResolveQuery:
            {
                struct A { ID3D12QueryHeap* h; D3D12_QUERY_TYPE t; UINT start, n; ID3D12Resource* dst; UINT64 offset; };
                const A& c = At<A>(a);
                l->ResolveQueryData(c.h, c.t, c.start, c.n, c.dst, c.offset);
                break;
            }
            case kClearRtv:
            {
                struct A { D3D12_CPU_DESCRIPTOR_HANDLE rtv; FLOAT c[4]; UINT n; };
                const A& c = At<A>(a);
                l->ClearRenderTargetView(c.rtv, c.c, c.n, c.n ? reinterpret_cast<const D3D12_RECT*>(a + sizeof(A)) : nullptr);
                break;
            }
            case kDrawIndexed:
            {
                const UINT* d = reinterpret_cast<const UINT*>(a);
                l->DrawIndexedInstanced(d[0], d[1], d[2], INT(d[3]), d[4]);
                break;
            }
            case kIndexBuffer: l->IASetIndexBuffer(reinterpret_cast<const D3D12_INDEX_BUFFER_VIEW*>(a)); break;
            case kCopyBuffer:
            {
                struct A { ID3D12Resource* dst; UINT64 dstOffset; ID3D12Resource* src; UINT64 srcOffset, bytes; };
                const A& c = At<A>(a);
                l->CopyBufferRegion(c.dst, c.dstOffset, c.src, c.srcOffset, c.bytes);
                break;
            }
            case kComputeRootSignature: l->SetComputeRootSignature(At<ID3D12RootSignature*>(a)); break;
            case kComputeTable:
            {
                struct A { UINT i; D3D12_GPU_DESCRIPTOR_HANDLE h; };
                l->SetComputeRootDescriptorTable(At<A>(a).i, At<A>(a).h);
                break;
            }
            case kComputeRoot32:
            {
                struct A { UINT i, n, offset; };
                const A& c = At<A>(a);
                l->SetComputeRoot32BitConstants(c.i, c.n, a + sizeof(A), c.offset);
                break;
            }
            case kDispatch:
            {
                const UINT* d = reinterpret_cast<const UINT*>(a);
                l->Dispatch(d[0], d[1], d[2]);
                break;
            }
            case kCreateRtv:
            {
                struct A { ID3D12Device* d; ID3D12Resource* r; D3D12_CPU_DESCRIPTOR_HANDLE h; };
                const A& c = At<A>(a);
                c.d->CreateRenderTargetView(c.r, nullptr, c.h);
                break;
            }
            case kCreateDsv:
            {
                struct A { ID3D12Device* d; ID3D12Resource* r; D3D12_DEPTH_STENCIL_VIEW_DESC desc; D3D12_CPU_DESCRIPTOR_HANDLE h; };
                const A& c = At<A>(a);
                c.d->CreateDepthStencilView(c.r, &c.desc, c.h);
                break;
            }
            case kClearDsv:
            {
                struct A { D3D12_CPU_DESCRIPTOR_HANDLE dsv; D3D12_CLEAR_FLAGS flags; FLOAT depth; UINT8 stencil; UINT n; };
                const A& c = At<A>(a);
                l->ClearDepthStencilView(c.dsv, c.flags, c.depth, c.stencil, c.n, c.n ? reinterpret_cast<const D3D12_RECT*>(a + sizeof(A)) : nullptr);
                break;
            }
            }
        }
    }
};
