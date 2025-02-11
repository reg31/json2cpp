
# json2cpp

![CI](https://github.com/lefticus/json2cpp/workflows/ci/badge.svg)
[![codecov](https://codecov.io/gh/lefticus/json2cpp/branch/main/graph/badge.svg)](https://codecov.io/gh/lefticus/json2cpp)
[![Language grade: C++](https://img.shields.io/lgtm/grade/cpp/github/lefticus/json2cpp)](https://lgtm.com/projects/g/lefticus/json2cpp/context:cpp)

**json2cpp** compiles a json file into `static constexpr` data structures that can be used at compile time or runtime.

Features

 * Literally 0 runtime overhead for loading the statically compiled JSON resource
 * Fully constexpr capable if you want to make compile-time decisions based on the JSON resource file
 * A `.cpp` firewall file is provided for you, if you have a large resource and don't want to pay the cost of compiling it more than once (but for normal size files it is VERY fast to compile, they are just data structures)
 * [nlohmann::json](https://github.com/nlohmann/json) compatible API (should be a drop-in replacement, some features might still be missing)
 * [valijson](https://github.com/tristanpenman/valijson) adapter file provided


See the [test](test) folder for examples for building resources, using the valijson adapter, constexpr usage of resources, and firewalled usage of resources.

This is a fork of the amazing [json2cpp](https://github.com/lefticus/json2cpp) with the following changes:

 - c++20 is the minimum required
 - respect order of insered [object properties](https://json.nlohmann.me/api/ordered_json/)
 - updated dependencies
 - support using contains() on arrays
 - fix [negative integer values not handled properly](https://github.com/lefticus/json2cpp/issues/18)
 - utf16 support for compatibility with QT (QString/QstringView)*


**Usage**

Have your json file in the same folder as the json2cpp executable and tpye the command:

    json2cpp "outputCppClassName" "yourJsonFile.json" "outputFolderPath/baseFilename"

I advise outputCppClassName and baseFilename to be the same for consistency, e.g.:

    json2cpp "myClass" "myFile.json" "./myClass"

This will generate 3 files you can include in your project:
myClass.cpp
myClass.hpp
myClass_impl.cpp


**utf16 support**

Set #DEFINE **JSON2CPP_USE_UTF16** in your project to compile as utf16 string views (char16_t) instead of utf8, this allows implicit conversion to QStringView or even to build a QString.
