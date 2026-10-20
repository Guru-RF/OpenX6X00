# Building outside buildroot

`build.sh` compiles the GUI with the cross compiler and the libraries of a
buildroot tree that has already been built once (`../AetherX6100Buildroot`
next to this repository, set in `conf.sh`). The result lands in
`buildroot/build/`. It is quicker than rebuilding the buildroot package when
chasing a compile error.

```
cd buildroot
./build.sh
```

`bt_start.sh` replaces buildroot's own copy in the image (see the System
cleanup step in `.github/workflows/main.yml`).
