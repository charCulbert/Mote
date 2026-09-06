#include "Plugin.h"

extern "C"
{
const CLAP_EXPORT clap_plugin_entry_t clap_entry {
    CLAP_VERSION,
    mote::entryInit,
    mote::entryDeinit,
    mote::entryGetFactory
};
}
