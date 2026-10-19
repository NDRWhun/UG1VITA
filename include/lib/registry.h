#ifndef LIB_REGISTRY_H_
#define LIB_REGISTRY_H_

#include <lib/winapp.h>
#include <string>
#include <vector>

namespace win32
{

static const x86::reg32 HKEY_CLASSES_ROOT = 0x80000000;
static const x86::reg32 HKEY_CURRENT_USER = 0x80000001;
static const x86::reg32 HKEY_LOCAL_MACHINE = 0x80000002;
static const x86::reg32 HKEY_USERS = 0x80000003;
static const x86::reg32 HKEY_CURRENT_CONFIG = 0x80000005;

// The guest registry: a tree of keys with typed values, names compared without case. Kept in a text file that is
// rewritten after every change, so values the game writes survive. Any thread.
namespace registry
{

enum ValueType
{
    REG_NONE    = 0,
    REG_SZ      = 1,
    REG_BINARY  = 3,
    REG_DWORD   = 4
};

struct Key;

// Reads the file (a missing file is an empty registry); later changes are written back to it.
void load(const char* path);
// Root key of a predefined handle (HKEY_*), or nullptr.
Key* root(x86::reg32 handle);
// Key below parent by a path of names separated by backslashes; created when create is set. nullptr if missing.
Key* find(Key* parent, const char* path, bool create);
// Stores a value and saves the file.
void setValue(Key* key, const std::string& name, x86::reg32 type, const void* data, x86::reg32 size);
// Stores a value unless the key has one of that name (defaults); saves the file when it adds one.
void setDefault(Key* key, const std::string& name, x86::reg32 type, const void* data, x86::reg32 size);
// RegQueryValueExA: ERROR_SUCCESS, ERROR_FILE_NOT_FOUND (2) or ERROR_MORE_DATA (234).
x86::reg32 query(Key* key, const std::string& name, x86::reg32* type, void* data, x86::reg32* size);

}

// A guest key handle (RegOpenKeyExA, RegCreateKeyExA); the predefined roots have fixed handles.
class RegistryKey : public GenericResource
{
public:
    explicit RegistryKey(registry::Key* key) : m_key(key) {}
    registry::Key* key() const { return m_key; }

private:
    registry::Key* m_key;
};

}

#endif
