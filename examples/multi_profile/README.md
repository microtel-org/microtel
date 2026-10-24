# `multi_profile`

Two independent named providers in one process, plus the registry that lets
code find one of them without being handed a pointer.

The example builds a `"frontend"` and a `"backend"` profile with different
service names and different samplers, and emits spans from both. A function
that receives no arguments then looks the backend up by name. After that it
walks through the registry rules that tend to trip people up: a duplicate name
is refused, `Shutdown` does not release a name, and destruction does.

## Run it

```bash
examples/stack/up.sh

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build --target microtel_example_multi_profile

./build/examples/microtel_example_multi_profile
```

```
endpoint: http://localhost:4317

emitting
  microtel-frontend-example trace_id: c2d15d47411fdb4ff46217b1f6686701
  backend.healthz sampled: no  (the backend sampler drops it)
  microtel-backend-example trace_id: 9f24b41fb453eaf8e67783a59e3bc3af

the registry
  GetProvider("frontend"): live
  GetProvider("backend"):  live
  GetProvider() ["default"]: nullptr  (nothing was built under it)
  Build() with WithProfileName("backend") refused: a live provider is already registered under profile name 'backend'; shutting a provider down does not release its name, destroying it does
  kind is DuplicateProfileName: yes  (the live provider keeps the name; this is never last-wins)

shutting down
  frontend: ForceFlush=Completed batches_sent=1 batches_failed=0 rejected=0 queue_depth=0 Shutdown=Completed
  backend: ForceFlush=Completed batches_sent=1 batches_failed=0 rejected=0 queue_depth=0 Shutdown=Completed
  after Shutdown, GetProvider("frontend"): live  (a name is freed by destruction, not by Shutdown)
  after destruction, GetProvider("frontend"): nullptr
  GetProvider("backend"):  live  (still owned here)

Both services should now appear in Grafana's service list:
  { resource.service.name = "microtel-frontend-example" }
  { resource.service.name = "microtel-backend-example" }
```

The endpoint defaults to the shared stack's OTLP/gRPC receiver; pass a
different one as the first argument.

## Two profiles, nothing shared

```cpp
auto frontend = microtel::SdkBuilder{}
                    .WithServiceName("microtel-frontend-example")
                    .WithProfileName("frontend")
                    .WithSampler(microtel::MakeAlwaysOnSampler())
                    .Build();

auto backend = microtel::SdkBuilder{}
                   .WithServiceName("microtel-backend-example")
                   .WithProfileName("backend")
                   .WithSampler(microtel::MakeSpanNameRuleSampler(
                       "backend.healthz",
                       microtel::MakeAlwaysOffSampler(),    // on match
                       microtel::MakeAlwaysOnSampler()))    // on everything else
                   .Build();
```

Each profile is a complete pipeline of its own. It owns its endpoint, protocol,
TLS material, sampler, `Resource`, processors, worker threads, transport and
I/O thread, and shares none of them with any other profile
([ICP 0027](../../docs/icps/0027-multi-profile-threading.md)). Both profiles
export to the same collector here, which is the least interesting thing they
could do. In practice you would point one profile at your own collector and
another at a vendor, or run a heavily sampled high-volume profile next to an
audit profile that keeps everything.

The samplers differ so you can see the independence in the output. The
frontend keeps everything it starts; the backend keeps everything except its
health check. `backend.healthz` is dropped at `StartSpan` and never reaches a
queue. The console line reports that, and Tempo agrees.

`WithProfileName` compares names byte for byte, with no normalisation and no
case folding, so `"backend"` and `"Backend"` are two different profiles.

## Finding a profile from code that was handed nothing

```cpp
std::string RecordBackendWork()
{
    microtel::Provider* const provider = microtel::GetProvider("backend");
    if (provider == nullptr)
    {
        return {};   // nothing live under that name
    }
    const auto tracer = provider->GetTracer("backend.library", "1.0.0");
    ...
}
```

This is why the registry exists. If your host builds its profiles in one place,
pass the `shared_ptr<Provider>` around and don't call `GetProvider` at all.
The lookup is for instrumentation you can't reach that way: library code, a
plugin, a signal-handler-adjacent callback, anything whose signature you don't
control.

`GetProvider()` with no argument is the default-profile accessor. It looks up
`"default"`, which is the name a provider gets when it is built without
`WithProfileName`. This example names both of its profiles, so the default slot
is empty and the call returns `nullptr`, as the run shows.

## Registry semantics

`GetProvider` returns a borrowed, non-owning pointer. It never transfers
ownership, and a lookup never extends a provider's lifetime. The owner is
whoever holds the `shared_ptr<Provider>` that `Build()` returned.

| Event | `GetProvider(name)` | Why |
|---|---|---|
| After `Build()` | the provider | registered by `Build`, under that name |
| After `Shutdown()` | **still the provider** | `Shutdown` does not deregister; every method on it now returns `Status::AlreadyShutDown` |
| After destruction | `nullptr` | the destructor releases the slot, before it tears anything down |
| In a `fork()` child | `nullptr` | every slot is emptied in the child, so it can re-`Build()` under the same names |

The run shows rows two and three back to back. Both providers are still `live`
after they have been shut down. The frontend goes to `nullptr` as soon as its
last `shared_ptr` is dropped, while the backend, which `main` still holds,
stays `live`.

Like every non-owning pointer in this API, the one `GetProvider` hands you
dangles once the provider is destroyed. The guarantee is that a later lookup
returns `nullptr` instead of the destroyed object. If your program destroys
providers while other threads are looking them up, you have to synchronise
that yourself. The intended pattern (look profiles up after building them,
hold the pointer, destroy at process teardown) never runs into the race.

There is one more lifetime rule the example has to respect, and the header
currently documents it wrongly
([#285](https://github.com/microtel-org/microtel/issues/285)): a `Tracer`
borrows its provider's pipeline through raw pointers and must not outlive the
provider. That is why the frontend's tracer goes out of scope before
`frontend.reset()`.

### Duplicate names fail loudly

```
Build() with WithProfileName("backend") refused: a live provider is already registered
under profile name 'backend'; shutting a provider down does not release its name,
destroying it does
```

The live provider keeps the name, and the new `Build()` is the one that fails,
with `ConfigError::Kind::DuplicateProfileName`. The second build never
replaces the first. If two threads call `Build()` with the same name at the
same time, you get exactly one provider and exactly one error. The registry
settles the race by lowest slot index, so the outcome is deterministic and
doesn't depend on which thread got there first.

The related failure is `ConfigError::Kind::ProfileLimitExceeded`. A process can
hold only a fixed number of live providers. That number is internal, so it
appears in the error message and not in any public header; raising it takes a
rebuild, and no ICP.

## What Grafana shows

Both services appear in the service list, side by side:

```
microtel-backend-example
microtel-frontend-example
```

In **Explore → Tempo**, run one query for each:

```
{ resource.service.name = "microtel-frontend-example" }   # 1 trace: frontend.request
{ resource.service.name = "microtel-backend-example" }    # 1 trace: backend.query
{ name = "backend.healthz" }                              # 0 — dropped at StartSpan
```

Two service names from one process is what a second profile gets you that a
configuration change can't. `service.name` is part of the `Resource`, the
`Resource` is fixed at `Build()`, and none of the four hot-reload setters
touches it, so a single provider can't be reconfigured into this.

## What to notice in the code

`GetProvider` returns a raw pointer, and you must check it for null. A name
that was never built, a provider that has already been destroyed, and a
`fork()`ed child all give you `nullptr`.

Each provider is shut down explicitly, in the order the program chooses. The
destructors would do it too, with a small finite timeout, but then teardown
follows member-destruction order and you can't see the flush statuses.

The duplicate-name attempt is real code. It builds, it fails, and the program
prints the failure. An example that only claims an error case in a comment
stops being accurate the day that error changes.
