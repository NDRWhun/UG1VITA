#include <lib/registry.h>
#include <lib/guest.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <strings.h>

namespace win32 { namespace registry
{

struct Value
{
    std::string             name;
    x86::reg32              type;
    std::vector<x86::reg8>  data;
};

struct Key
{
    std::string                         name;
    Key*                                parent;
    std::vector<std::unique_ptr<Key>>   children;
    std::vector<Value>                  values;
};

namespace
{

const x86::reg32 ERROR_SUCCESS = 0;
const x86::reg32 ERROR_FILE_NOT_FOUND = 2;
const x86::reg32 ERROR_MORE_DATA = 234;

struct Root
{
    x86::reg32  handle;
    const char* name;
};

const Root s_rootNames[] =
{
    { HKEY_CLASSES_ROOT,   "HKEY_CLASSES_ROOT" },
    { HKEY_CURRENT_USER,   "HKEY_CURRENT_USER" },
    { HKEY_LOCAL_MACHINE,  "HKEY_LOCAL_MACHINE" },
    { HKEY_USERS,          "HKEY_USERS" },
    { HKEY_CURRENT_CONFIG, "HKEY_CURRENT_CONFIG" },
};
const size_t s_rootCount = sizeof(s_rootNames) / sizeof(s_rootNames[0]);

Key s_roots[s_rootCount];
pf::LwMutex s_mutex("registry", true);
std::string s_path;

Key* child(Key* parent, const std::string& name, bool create)
{
    for (const std::unique_ptr<Key>& k : parent->children)
    {
        if (strcasecmp(k->name.c_str(), name.c_str()) == 0)
            return k.get();
    }
    if (!create)
        return nullptr;
    parent->children.emplace_back(new Key{name, parent, {}, {}});
    return parent->children.back().get();
}

Value* findValue(Key* key, const std::string& name)
{
    for (Value& v : key->values)
    {
        if (strcasecmp(v.name.c_str(), name.c_str()) == 0)
            return &v;
    }
    return nullptr;
}

std::string pathOf(const Key* key)
{
    std::string path;
    for (; key->parent; key = key->parent)
        path = "\\" + key->name + path;
    return key->name + path;
}

// One section per key with values: "[path]", then "name<TAB>type<TAB>hex bytes" lines.
void writeKey(FILE* file, const Key* key)
{
    if (!key->values.empty())
    {
        fprintf(file, "[%s]\n", pathOf(key).c_str());
        for (const Value& v : key->values)
        {
            fprintf(file, "%s\t%u\t", v.name.c_str(), unsigned(v.type));
            for (x86::reg8 b : v.data)
                fprintf(file, "%02x", b);
            fputc('\n', file);
        }
    }
    for (const std::unique_ptr<Key>& k : key->children)
        writeKey(file, k.get());
}

void save()
{
    if (s_path.empty())
        return;
    FILE* file = fopen(s_path.c_str(), "w");
    if (!file)
    {
        pf::logError("registry: cannot write %s", s_path.c_str());
        return;
    }
    for (const Key& root : s_roots)
        writeKey(file, &root);
    fclose(file);
}

void store(Key* key, const std::string& name, x86::reg32 type, const void* data, x86::reg32 size)
{
    Value* v = findValue(key, name);
    if (!v)
    {
        key->values.push_back(Value{name, type, {}});
        v = &key->values.back();
    }
    v->type = type;
    const x86::reg8* bytes = static_cast<const x86::reg8*>(data);
    v->data.assign(bytes, bytes + size);
}

}

static void loadFile(const char* path);

// Values a retail install provides that the game reads (docs/re/boot.md section 5, cpu_threads.md section 1.3);
// the file's own values win. Not seeded on purpose: "CD Drive" (a failed open would then retry on a CD and run the
// insert-disc loop) and "Install Dir" (it only feeds the unused file cache); the cache sizes and
// StreamingInstall, whose defaults the game handles. ~MHz matches the 500 MHz time stamp counter, so the EA
// library does not measure the clock with a one-second busy wait.
static void seedDefaults()
{
    static const char language[] = "English US";
    Key* game = find(root(HKEY_LOCAL_MACHINE), "Software\\EA Games\\Need For Speed Underground", true);
    setDefault(game, "Language", REG_SZ, language, sizeof(language));
    const x86::reg32 mhz = x86::TimestampMhz;
    Key* cpu = find(root(HKEY_LOCAL_MACHINE), "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", true);
    setDefault(cpu, "~MHz", REG_DWORD, &mhz, sizeof(mhz));
}

void load(const char* path)
{
    loadFile(path);
    seedDefaults();
}

static void loadFile(const char* path)
{
    pf::LwLockGuard guard(s_mutex);
    for (size_t i = 0; i < s_rootCount; ++i)
    {
        s_roots[i].name = s_rootNames[i].name;
        s_roots[i].parent = nullptr;
    }
    s_path = path;
    FILE* file = fopen(path, "r");
    if (!file)
        return;
    Key* key = nullptr;
    char line[4096];
    while (fgets(line, sizeof(line), file))
    {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[')
        {
            char* end = strchr(line, ']');
            if (end)
                *end = 0;
            char* rest = strchr(line + 1, '\\');
            if (rest)
                *rest++ = 0;
            key = nullptr;
            for (size_t i = 0; i < s_rootCount; ++i)
            {
                if (strcmp(s_rootNames[i].name, line + 1) == 0)
                    key = find(&s_roots[i], rest ? rest : "", true);
            }
            continue;
        }
        char* type = strchr(line, '\t');
        char* hex = type ? strchr(type + 1, '\t') : nullptr;
        if (!key || !hex)
            continue;
        *type++ = 0;
        *hex++ = 0;
        std::vector<x86::reg8> data;
        for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
        {
            const char pair[3] = { hex[i], hex[i + 1], 0 };
            data.push_back(x86::reg8(strtoul(pair, nullptr, 16)));
        }
        store(key, line, x86::reg32(strtoul(type, nullptr, 10)), data.data(), x86::reg32(data.size()));
    }
    fclose(file);
    pf::logInfo("registry: loaded %s", path);
}

Key* root(x86::reg32 handle)
{
    for (size_t i = 0; i < s_rootCount; ++i)
    {
        if (s_rootNames[i].handle == handle)
            return &s_roots[i];
    }
    return nullptr;
}

Key* find(Key* parent, const char* path, bool create)
{
    pf::LwLockGuard guard(s_mutex);
    Key* key = parent;
    while (key && path && *path)
    {
        const char* end = strchr(path, '\\');
        const std::string name = end ? std::string(path, end) : std::string(path);
        if (!name.empty())
            key = child(key, name, create);
        path = end ? end + 1 : nullptr;
    }
    return key;
}

void setValue(Key* key, const std::string& name, x86::reg32 type, const void* data, x86::reg32 size)
{
    pf::LwLockGuard guard(s_mutex);
    store(key, name, type, data, size);
    save();
}

void setDefault(Key* key, const std::string& name, x86::reg32 type, const void* data, x86::reg32 size)
{
    pf::LwLockGuard guard(s_mutex);
    if (findValue(key, name))
        return;
    store(key, name, type, data, size);
    save();
}

// Without data only the type and size are returned; data that does not fit gives ERROR_MORE_DATA with the size
// needed.
x86::reg32 query(Key* key, const std::string& name, x86::reg32* type, void* data, x86::reg32* size)
{
    pf::LwLockGuard guard(s_mutex);
    const Value* v = findValue(key, name);
    if (!v)
        return ERROR_FILE_NOT_FOUND;
    if (type)
        *type = v->type;
    if (!size)
        return ERROR_SUCCESS;
    const x86::reg32 room = *size;
    *size = x86::reg32(v->data.size());
    if (!data)
        return ERROR_SUCCESS;
    if (room < v->data.size())
        return ERROR_MORE_DATA;
    memcpy(data, v->data.data(), v->data.size());
    return ERROR_SUCCESS;
}

}}
