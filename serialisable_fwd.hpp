#ifndef SERIALISABLE_FWD_HPP_INCLUDED
#define SERIALISABLE_FWD_HPP_INCLUDED

#include <stdint.h>
#include <vector>
#include <nlohmann/json.hpp>

#define SERIALISE_SIGNATURE() static inline uint32_t id_counter = 0;\
std::vector<size_t> last_ratelimit_time; \
void _internal_helper(){}\
using self_t = typename class_extractor<decltype(&_internal_helper)>::class_t;\
void serialise(serialise_context& ctx, nlohmann::json& data, self_t* other = nullptr)

template<typename T>
struct class_extractor;

template<typename C, typename R, typename... Args>
struct class_extractor<R(C::*)(Args...)>
{
    using class_t = C;
};

struct serialise_context;

size_t get_next_persistent_id();

struct owned
{
    size_t _pid = get_next_persistent_id();
};

struct serialisable
{
    static size_t time_ms();

    virtual ~serialisable();
};

#endif // SERIALISABLE_FWD_HPP_INCLUDED
