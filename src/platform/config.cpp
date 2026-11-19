#include <platform/config.h>
#include <platform/log.h>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <string>

namespace pf
{

namespace
{

const int MaxEntries = 64;
const int MaxKey     = 32;
const int MaxValue   = 64;
const int MaxLine    = 256;

// Every setting the game reads; other keys are reported as unknown.
const char* const Known[] =
{
    // Player settings.
    "widescreen", "hud_widescreen", "menu_widescreen", "button_prompts", "fit_4_3", "frame_rate", "car_reflection_interval", "rear_touch",
    "garage_camera",
    // Cores, logs and the benchmark.
    "use_core3", "core3_tasks", "timing_log", "timing_phases", "hitch_log", "console_log",
    "benchmark", "benchmark_exit", "benchmark_seconds", "benchmark_laps", "benchmark_opponents", "benchmark_track",
    "benchmark_timing_log", "benchmark_mode", "benchmark_car", "benchmark_driver",
    // Renderer.
    "alpha_draw_order", "skip_unused_alpha_test", "matrices_on_render_thread", "texture_cache_mb", "texture_async",
    "gxm_precomputed", "gxm_lists", "gxm_core2", "gxm_program_list", "cull_swap", "debug_frame_dump",
    // Native versions of guest code and their shadow compares.
    "bank_native", "shadow_bank", "coll_native", "shadow_coll", "cull_native", "cull_parallel", "shadow_cull",
    "draw_native", "shadow_draw", "flush_native", "flush_async", "flush_async_core", "flush_async_priority",
    "flush_async_frame", "flares_native", "flares_parallel", "shadow_flares",
    "load_native", "shadow_load", "math_native", "shadow_math", "particles_native", "particles_update",
    "particles_parallel", "particles_direct", "shadow_particles", "prep_native", "shadow_prep",
};

bool isKnown(const char* key)
{
    for (const char* known : Known)
    {
        if (strcmp(known, key) == 0)
            return true;
    }
    return false;
}

struct Entry
{
    char key[MaxKey];
    char value[MaxValue];
};

Entry s_entries[MaxEntries];
int   s_count = 0;
std::string s_path;

const char* find(const char* key)
{
    for (int i = 0; i < s_count; ++i)
    {
        if (strcmp(s_entries[i].key, key) == 0)
            return s_entries[i].value;
    }
    return nullptr;
}

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Trims spaces and line ends in place; returns the trimmed start.
char* trim(char* text)
{
    while (isSpace(*text))
        text++;
    char* end = text + strlen(text);
    while (end > text && isSpace(end[-1]))
        *--end = 0;
    return text;
}

// Reads one line into line; false at the end of the file. A line longer than the buffer is consumed whole and
// returned as too long.
bool readLine(FILE* file, char* line, int size, bool* tooLong)
{
    if (!fgets(line, size, file))
        return false;
    *tooLong = false;
    if (!strchr(line, '\n') && !feof(file))
    {
        *tooLong = true;
        int c;
        while ((c = fgetc(file)) != EOF && c != '\n')
        {
        }
    }
    return true;
}

}

void Config::load(const char* path)
{
    s_count = 0;
    s_path = path;
    FILE* file = fopen(path, "rt");
    if (!file)
        return;
    char buffer[MaxLine];
    bool tooLong = false;
    for (int number = 1; readLine(file, buffer, sizeof(buffer), &tooLong); ++number)
    {
        char* line = buffer;
        if (number == 1 && strncmp(line,"\xef\xbb\xbf", 3) == 0)
            line += 3;
        if (tooLong)
        {
            logError("config line %d: longer than %d characters, ignored", number, MaxLine - 2);
            continue;
        }
        line = trim(line);
        if (!line[0] || line[0] == '#')
            continue;
        char* separator = strchr(line, '=');
        if (!separator)
        {
            logError("config line %d: no '=', ignored", number);
            continue;
        }
        *separator = 0;
        const char* key = trim(line);
        const char* value = trim(separator + 1);
        if (!key[0] || strlen(key) >= MaxKey || strlen(value) >= MaxValue)
        {
            logError("config line %d: empty key, key over %d or value over %d characters, ignored", number,
                     MaxKey - 1, MaxValue - 1);
            continue;
        }
        const bool known = isKnown(key);
        if (find(key))
        {
            if (known)
                logError("config line %d: %s repeated, first value kept", number, key);
            continue;
        }
        if (s_count == MaxEntries)
        {
            logError("config line %d: more than %d entries, %s ignored", number, MaxEntries, key);
            continue;
        }
        // Unknown keys are kept too, so that a repeat is not reported again; nothing reads them.
        Entry& entry = s_entries[s_count++];
        strcpy(entry.key, key);
        strcpy(entry.value, value);
        if (known)
            logInfo("config %s=%s", entry.key, entry.value);
        else
            logError("config: unknown setting '%s'", entry.key);
    }
    fclose(file);
}

bool Config::setBool(const char* key, bool value)
{
    if (s_path.empty() || !isKnown(key) || strlen(key) >= MaxKey)
        return false;
    int index = 0;
    while (index < s_count && strcmp(s_entries[index].key, key) != 0)
        ++index;
    if (index == MaxEntries)
        return false;
    std::string input;
    if (FILE* file = fopen(s_path.c_str(), "rb"))
    {
        char block[4096];
        size_t length;
        while ((length = fread(block, 1, sizeof(block), file)) != 0)
            input.append(block, length);
        const bool failed = ferror(file);
        fclose(file);
        if (failed)
            return false;
    }
    else if (errno != ENOENT)
        return false;

    const char* text = value ? "on" : "off";
    std::string output;
    bool found = false;
    for (size_t at = 0; at < input.size();)
    {
        const size_t newline = input.find('\n', at);
        const size_t end = newline == std::string::npos ? input.size() : newline + 1;
        const size_t separator = input.find('=', at);
        std::string name = separator < end ? input.substr(at, separator - at) : "";
        if (at == 0 && name.compare(0, 3, "\xef\xbb\xbf") == 0)
            name.erase(0, 3);
        if (separator < end && strcmp(trim(&name[0]), key) == 0)
        {
            output.append(input, at, separator + 1 - at);
            output += text;
            if (end > at && input[end - 1] == '\n')
                output += end > at + 1 && input[end - 2] == '\r' ? "\r\n" : "\n";
            found = true;
        }
        else
            output.append(input, at, end - at);
        at = end;
    }
    if (!found)
    {
        if (!output.empty() && output.back() != '\n')
            output += '\n';
        output += std::string(key) + "=" + text + "\n";
    }
    const std::string temporary = s_path + ".tmp";
    FILE* file = fopen(temporary.c_str(), "wb");
    if (!file)
        return false;
    bool ok = fwrite(output.data(), 1, output.size(), file) == output.size();
    if (fflush(file) != 0)
        ok = false;
    if (fclose(file) != 0)
        ok = false;
    if (!ok || rename(temporary.c_str(), s_path.c_str()) != 0)
    {
        remove(temporary.c_str());
        logError("config: could not save %s", key);
        return false;
    }
    if (index == s_count)
        ++s_count;
    strcpy(s_entries[index].key, key);
    strcpy(s_entries[index].value, text);
    logInfo("config saved %s=%s", key, text);
    return true;
}

bool Config::tryGetBool(const char* key, bool* value)
{
    const char* text = find(key);
    if (!text)
        return false;
    static const char* const On[] = { "on", "true", "yes", "1" };
    static const char* const Off[] = { "off", "false", "no", "0" };
    for (int i = 0; i < 4; ++i)
    {
        if (strcasecmp(text, On[i]) == 0 || strcasecmp(text, Off[i]) == 0)
        {
            *value = strcasecmp(text, On[i]) == 0;
            return true;
        }
    }
    logError("config %s=%s is not a boolean (on/off, true/false, yes/no, 1/0), ignored", key, text);
    return false;
}

bool Config::tryGetInt(const char* key, int* value)
{
    const char* text = find(key);
    if (!text)
        return false;
    char* end = nullptr;
    errno = 0;
    const long number = strtol(text, &end, 10);
    if (end == text || *end || errno == ERANGE || number < INT_MIN || number > INT_MAX)
    {
        logError("config %s=%s is not an integer, ignored", key, text);
        return false;
    }
    *value = int(number);
    return true;
}

bool Config::getBool(const char* key, bool defaultValue)
{
    bool value = defaultValue;
    return tryGetBool(key, &value) ? value : defaultValue;
}

int Config::getInt(const char* key, int defaultValue)
{
    int value = defaultValue;
    return tryGetInt(key, &value) ? value : defaultValue;
}

const char* Config::getString(const char* key, const char* defaultValue)
{
    const char* value = find(key);
    return value ? value : defaultValue;
}

int Config::getChoice(const char* key, const char* const* names, int count, int defaultIndex)
{
    const char* text = find(key);
    if (!text)
        return defaultIndex;
    for (int i = 0; i < count; ++i)
    {
        if (strcasecmp(text, names[i]) == 0)
            return i;
    }
    char list[MaxLine] = "";
    for (int i = 0; i < count; ++i)
    {
        if (i)
            strncat(list, "/", sizeof(list) - strlen(list) - 1);
        strncat(list, names[i], sizeof(list) - strlen(list) - 1);
    }
    logError("config %s=%s is not one of %s, %s used", key, text, list, names[defaultIndex]);
    return defaultIndex;
}

}
