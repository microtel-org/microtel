| Std | Feature | transport (files / uses) | wire (files / uses) | exporter (files / uses) | recording path (files / uses) | Note |
|---|---|---:|---:|---:|---:|---|
| C++20 | concepts / requires | – | – | – | – |  |
| C++20 | std::span | 8 / 19 | 12 / 31 | – | 2 / 2 |  |
| C++20 | ranges / views | 3 / 12 | 1 / 1 | – | 1 / 1 |  |
| C++20 | <bit> (bit_cast, endian, countl_zero, ...) | – | – | – | – |  |
| C++20 | designated initializers | 7 / 160 | 9 / 266 | 5 / 17 | 3 / 24 | aggregate init `{.field = ...}`; C++14 needs positional init or setters |
| C++20 | coroutines | – | – | – | – |  |
| C++20 | three-way comparison / defaulted == | – | – | – | – |  |
| C++20 | consteval / constinit | – | – | – | – |  |
| C++20 | std::jthread / stop_token | – | – | – | – |  |
| C++20 | <format> | – | – | – | – |  |
| C++20 | [[likely]] / [[unlikely]] | – | – | – | – |  |
| C++20 | using enum | – | – | – | – |  |
| C++20 | template lambdas | – | – | – | – |  |
| C++20 | starts_with / ends_with / contains | 2 / 3 | – | – | – | string_view/string/map members |
| C++20 | std::erase / erase_if | – | – | – | – |  |
| C++20 | std::remove_cvref / type_identity | – | – | – | – |  |
| C++20 | std::ssize / to_array / midpoint | – | – | – | – |  |
| C++20 | atomic wait/notify, latch, barrier, atomic_ref | – | – | – | – | atomic wait under-counted: condition_variable wait/notify is C++11 and excluded |
| C++17 | std::optional | 4 / 18 | 10 / 90 | 4 / 5 | 1 / 1 |  |
| C++17 | std::string_view | 7 / 79 | 6 / 26 | 8 / 13 | 6 / 22 |  |
| C++17 | std::variant / visit | – | 2 / 5 | – | 3 / 8 |  |
| C++17 | if constexpr | – | 1 / 13 | – | – |  |
| C++17 | structured bindings | 1 / 1 | 1 / 2 | – | 1 / 1 |  |
| C++17 | if / switch with initializer | 2 / 2 | 1 / 2 | – | 1 / 1 | approximate |
| C++17 | nested namespace a::b | 17 / 17 | 21 / 21 | 11 / 11 | 9 / 9 |  |
| C++17 | inline variables | – | 1 / 2 | 1 / 2 | – |  |
| C++17 | [[nodiscard]] / [[maybe_unused]] / [[fallthrough]] | 15 / 83 | 19 / 95 | 6 / 26 | 10 / 28 | attributes: removable, C++14 ignores-or-errors |
| C++17 | std::scoped_lock | 4 / 28 | 1 / 5 | 4 / 9 | 1 / 4 |  |
| C++17 | std::byte | 5 / 22 | 13 / 58 | – | – |  |
| C++17 | fold expressions | – | – | – | – |  |
| C++17 | CTAD-only spellings (scoped_lock/lock_guard/unique_lock without <>) | 4 / 31 | 1 / 5 | 4 / 19 | 1 / 5 |  |
| C++17 | std::invoke / apply / is_invocable | – | – | – | – |  |
| C++17 | std::filesystem | – | – | – | – |  |

| Area | Files | Lines (with comments) | Code lines (non-blank, comments stripped) |
|---|---:|---:|---:|
| transport | 17 | 5,288 | 3,775 |
| wire | 21 | 5,217 | 3,678 |
| exporter | 11 | 1,916 | 1,232 |
| recording path | 13 | 2,163 | 1,292 |
