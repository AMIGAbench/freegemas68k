/* Tiny stand-in for the part of jsoncpp that OptionsManager needs.
 * The options are stored as key=value lines (see options_amiga.cpp). */
#ifndef FG_AMIGA_JSON_H
#define FG_AMIGA_JSON_H

#include <map>
#include <string>
#include <vector>   /* (jsoncpp brings it in; go_window.h relies on that) */

namespace Json {
    class Value {
    public:
        Value() : mInt(0), mSet(false) {}
        Value(int v) : mInt(v), mSet(true) {}
        Value(bool v) : mInt(v ? 1 : 0), mSet(true) {}

        Value & operator[](const std::string & key) { return mMembers[key]; }
        Value & operator=(int v) { mInt = v; mSet = true; return *this; }
        Value & operator=(bool v) { mInt = v ? 1 : 0; mSet = true; return *this; }

        Value get(const std::string & key, const Value & def) const {
            std::map<std::string, Value>::const_iterator it = mMembers.find(key);
            return (it != mMembers.end() && it->second.mSet) ? it->second : def;
        }
        int asInt() const { return mInt; }
        bool asBool() const { return mInt != 0; }

        std::map<std::string, Value> mMembers;
        int mInt;
        bool mSet;
    };
}

#endif
