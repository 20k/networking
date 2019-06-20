#ifndef SERIALISABLE_FWD_HPP_INCLUDED
#define SERIALISABLE_FWD_HPP_INCLUDED

#include <stdint.h>
#include <vector>
#include <nlohmann/json_fwd.hpp>

#define SERIALISE_SIGNATURE(x) \
    static inline uint32_t id_counter = 0;\
    static inline uint32_t id_counter2 = 0;\
    void serialise(serialise_context& ctx, nlohmann::json& data, x* other = nullptr)

#define SERIALISE_SIGNATURE_SIMPLE(x) SERIALISE_SIGNATURE(x)

#define SERIALISE_SIGNATURE_NOSMOOTH(x) \
    static inline uint32_t id_counter = 0;\
    void serialise(serialise_context& ctx, nlohmann::json& data, x* other = nullptr)


#define DECLARE_SERIALISE_FUNCTION(x) \
void serialise_base(x* me, serialise_context& ctx, nlohmann::json& data, x* other)

#define SERIALISE_SETUP() static uint32_t id_counter = 0; static uint32_t id_counter2 = 0;


#define SERIALISE_BODY(x) void x::serialise(serialise_context& ctx, nlohmann::json& data, x* other)
#define SERIALISE_BODY_SIMPLE(x) SERIALISE_BODY(x)

struct serialise_context;

size_t& get_raw_id_impl();
size_t get_next_persistent_id();
void set_next_persistent_id(size_t in);

using pid_callback_t = void (*)(size_t current, size_t requested, void* udata);
void set_pid_callback(pid_callback_t callback);
void set_pid_udata(void* udata);

struct temporary_owned{};

struct owned
{
    size_t _pid = -1;

    owned(){_pid = get_next_persistent_id();}
    owned(temporary_owned tmp){}

    virtual ~owned() = default;
};

struct serialisable
{
    std::vector<size_t> last_ratelimit_time;

    virtual ~serialisable(){}
};

struct free_function
{

};

struct rate_limited
{

};

struct ts_vector;

struct smoothed
{
    std::vector<ts_vector> last_vals;
};

#endif // SERIALISABLE_FWD_HPP_INCLUDED
