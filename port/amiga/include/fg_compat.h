/* forced into every C++ file: what libnix's C++ headers lack */
#ifndef FG_COMPAT_H
#define FG_COMPAT_H
#ifdef __cplusplus
#include <cmath>
namespace std {
    using ::round;
    using ::lround;
    using ::trunc;
}
#endif
#endif
