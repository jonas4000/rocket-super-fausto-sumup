# PlatformIO pre-script: compile C++ as C++17 without passing -std=c++17 to C files.
Import("env")
env.Append(CXXFLAGS=["-std=c++17"])
