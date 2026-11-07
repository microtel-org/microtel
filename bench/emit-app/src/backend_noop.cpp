// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// No-op backend: the emit-app with tracing disabled. The control socket, the
// workload threads, their pacing, clock reads and histogram run as for every
// other backend, but no SDK is linked and nothing is emitted or exported. The
// bench driver's baseline-noop SUT measures the harness's own CPU with it
// (bench/docs/methodology.md, "CPU").

#include "backend.hpp"

namespace bench
{

namespace
{

class NoopBackend final : public IBackend
{
public:
    void Init(const BackendOptions& /*opts*/) override {}
    void EmitSpan() override {}
    void EmitRequest() override {}
    void Shutdown() override {}
    [[nodiscard]] BackendStats Stats() const override
    {
        return {};
    }
};

}  // namespace

IBackend* CreateBackend()
{
    return new NoopBackend();  // NOLINT(cppcoreguidelines-owning-memory)
}

}  // namespace bench
