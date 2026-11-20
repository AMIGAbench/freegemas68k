/* AmigaOS version string ("version Freegemas080 FULL"). Program name, version and
   date come from Makefile.amiga; the date is the day of the build. */
#ifndef FG_PROGNAME
#error "FG_PROGNAME, FG_VERSION and FG_DATE are set by Makefile.amiga"
#endif
#define FG_STR2(x) #x
#define FG_STR(x) FG_STR2(x)

extern "C" const char fg_verstag[] __attribute__((used)) =
    "$VER: " FG_STR(FG_PROGNAME) " " FG_STR(FG_VERSION) " (" FG_STR(FG_DATE) ") AmigaOS RTG port";
