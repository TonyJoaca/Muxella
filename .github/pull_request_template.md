## Description
Briefly describe the changes introduced in this pull request and the motivation behind them.

## Type of Change
- [ ] Bug fix (non-breaking change fixing an issue)
- [ ] New feature (non-breaking change adding functionality)
- [ ] Audio DSP / Core engine performance optimization
- [ ] Documentation update
- [ ] Code cleanliness / refactoring

## Clean-Room & Legal Compliance
- [ ] No proprietary, reverse-engineered, or decompiled code has been referenced or included.
- [ ] Any new third-party dependencies are permissively or compatibly licensed and documented.

## Testing & Verification
- [ ] Built with CMake (`cmake --build build --config Release`)
- [ ] All CTest unit tests pass (`ctest --test-dir build --output-on-failure`)
- [ ] Python GUI tests pass (`python tests/test_b_bus_gui.py`)
- [ ] Real-time safety verified (zero allocations on the active audio callback path)
