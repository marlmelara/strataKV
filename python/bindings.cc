#include <string>

#include <pybind11/pybind11.h>

#include "stratakv/version.h"

namespace py = pybind11;

// P0 exposes only the version, which is enough to prove the whole build chain
// works: CMake compiles the engine, scikit-build-core places the extension
// inside the Python package, and `import stratakv` loads it. The DB class
// arrives in P5.
PYBIND11_MODULE(_stratakv, m) {
  m.doc() = "Compiled core of StrataKV. Import the `stratakv` package instead.";

  m.def(
      "version", []() { return std::string(stratakv::VersionString()); },
      "Version of the compiled engine, taken from the CMake project version.");

  m.attr("__version__") = std::string(stratakv::VersionString());
}
