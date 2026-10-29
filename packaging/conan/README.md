# Conan 2 recipe for microtel

[`recipes/microtel/`](recipes/microtel/) is a Conan 2 recipe in
conan-center-index layout, ready to copy into a CCI fork. It isn't on Conan
Center yet; [`docs/packaging/PR_CONAN.md`](../../docs/packaging/PR_CONAN.md)
has the submission steps.

Until it is, build it into your local Conan cache from a microtel checkout:

```bash
conan create packaging/conan/recipes/microtel/all --version=1.2.5 \
      -s compiler.cppstd=20 --build=missing
```

Add `-o "microtel/*:leaf=True"` for the experimental C leaf
(`microtel::leaf`). Then require `microtel/1.2.5` in your project and use it
with `CMakeDeps`:

```cmake
find_package(microtel CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE microtel::microtel)
```

Linux only, with GCC 13+ or Clang 18+. At each release, add the new version
to `recipes/microtel/config.yml` and `recipes/microtel/all/conandata.yml`
(`docs/releasing.md` §4).
