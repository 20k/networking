#ifndef SERIALISABLE_MSGPACK_HPP_INCLUDED
#define SERIALISABLE_MSGPACK_HPP_INCLUDED

#include <msgpack.h>
#include <cstdint>
#include <cmath>
#include <vec/vec.hpp>
#include <vector>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>
#include <iostream>
#include <variant>
#include <set>

#include "serialisable_msgpack_fwd.hpp"

struct serialise_context_msgpack
{
    uint64_t pointer_id = 1;
    std::map<uintptr_t, uint64_t> serialising_pointers;
    std::map<uint64_t, uintptr_t> deserialising_pointers;

    bool encode = true;

    msgpack_sbuffer sbuf;
    msgpack_packer pk;

    void start()
    {
        msgpack_sbuffer_init(&sbuf);
        msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);
    }

    void stop()
    {
        msgpack_sbuffer_destroy(&sbuf);
    }
};

#define SETUP_MSG_FSERIALISE_SIMPLE(cnt) if(ctx.encode){ CHECK_THROW(msgpack_pack_map(&ctx.pk, cnt)); } int counter = 0;

#define DO_MSG_FSERIALISE(x, id, name) touch_member_base(ctx, obj, me.x, id, name)
#define DO_MSG_FSERIALISE_NAMED(x, name) touch_member_base(ctx, obj, me.x, counter++, name)
#define DO_MSG_FSERIALISE_SIMPLE(x) touch_member_base(ctx, obj, me.x, counter++, #x)

#ifdef SERIALISE_ALLOW_BOOST_PFR
#include <boost/pfr.hpp>

template< size_t I, size_t max_value, typename T, typename U>
inline
void reflect_on_aggregate(T& t, U&& func)
{
    func(boost::pfr::get<I>(t), boost::pfr::get_name<I, T>(), I);

    if constexpr(I+1 != max_value)
        reflect_on_aggregate<I+1, max_value>(t, std::forward<U>(func));
}

#define DEFINE_MSG_AGGREGATE_FSERIALISE(x) \
    void serialise_base(x& me, serialise_context_msgpack& ctx, msgpack_object* obj) { \
        constexpr size_t count = boost::pfr::tuple_size<x>::value; \
        \
        if(ctx.encode){ CHECK_THROW(msgpack_pack_map(&ctx.pk, count)); } \
        \
        reflect_on_aggregate<0, count>(me, [&ctx, obj](auto&& val, auto&& name, size_t idx) \
        { \
            touch_member_base(ctx, obj, val, idx, name); \
        }); \
    \
    }

#endif

#define CHECK_THROW(x) do{if(auto rval = (x); rval != 0) { throw std::runtime_error("Serialisation failed " + std::to_string(rval)); } } while(0)

template<typename T, typename = void>
struct has_serialisable_base_c : std::false_type{};

template<typename T>
struct has_serialisable_base_c<T, std::void_t<decltype(serialise_base(std::declval<T&>(), std::declval<serialise_context_msgpack&>(), std::declval<msgpack_object*>()))>> : std::true_type{};

template<typename T>
inline constexpr bool has_serialisable_base()
{
    return has_serialisable_base_c<T>::value;
}

void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::string& in);
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::string_view& in);
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, const char* in);
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, nlohmann::json& in);

template<typename... T>
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::variant<T...>& in);

template<int N, typename T>
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, vec<N, T>& in);

template<typename T>
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::optional<T>& in);

template<typename T, std::size_t N>
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::array<T, N>& in);

template<typename T, typename U>
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::pair<T, U>& in);

template<typename T>
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::vector<T>& in);

template<typename T>
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::set<T>& in);

template<typename T, typename U>
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::map<T, U>& in);

template<typename T, typename U>
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::unordered_map<T, U>& in);

template<typename T>
inline
void set_unfinite_to_0(T& in)
{
    if constexpr(std::is_floating_point_v<T>)
    {
        if(!std::isfinite(in))
        {
            in = 0;
        }
    }
}

template<typename T, typename name_type>
inline
void touch_member_base(serialise_context_msgpack& ctx, msgpack_object* obj, T& in, int id, name_type name)
{
    if(ctx.encode)
    {
        do_serialise(ctx, nullptr, name);
        do_serialise(ctx, nullptr, in);
    }
    else
    {
        in = T();

        auto is_eq = [&](const char* ptr, size_t size)
        {
            std::string_view view(ptr, size);

            if constexpr(std::is_same_v<name_type, std::string_view>)
            {
                return view == name;
            }
            else
            {
                std::string_view sname(name, strlen(name));

                return view == sname;
            }
        };

        if(id < (int)obj->via.map.size)
        {
            uint32_t len = obj->via.map.ptr[id].key.via.str.size;

            if(is_eq(obj->via.map.ptr[id].key.via.str.ptr, len))
            {
                do_serialise(ctx, &obj->via.map.ptr[id].val, in);

                return;
            }
        }

        for(int i=0; i < (int)obj->via.map.size; i++)
        {
            uint32_t len = obj->via.map.ptr[i].key.via.str.size;

            if(is_eq(obj->via.map.ptr[i].key.via.str.ptr, len))
            {
                do_serialise(ctx, &obj->via.map.ptr[i].val, in);

                return;
            }
        }
    }
}

namespace serialise_impl
{
    template <typename... Args>
    constexpr bool dependent_false = false;
}

template<typename T>
inline
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, T& in)
{
    if(ctx.encode)
    {
        if constexpr(std::is_base_of_v<serialise_msgpack, T> || has_serialisable_base<T>())
        {
            serialise_base(in, ctx, obj);
        }
        else
        {
            if constexpr(std::is_integral_v<T>)
            {
                constexpr bool sign = std::is_signed_v<T>;
                constexpr int width = sizeof(T);

                if constexpr(sign)
                {
                    if constexpr(width == 1)
                    {
                        CHECK_THROW(msgpack_pack_int8(&ctx.pk, in));
                    }
                    else if constexpr(width == 2)
                    {
                        CHECK_THROW(msgpack_pack_int16(&ctx.pk, in));
                    }
                    else if constexpr(width == 4)
                    {
                        CHECK_THROW(msgpack_pack_int32(&ctx.pk, in));
                    }
                    else if constexpr(width == 8)
                    {
                        CHECK_THROW(msgpack_pack_int64(&ctx.pk, in));
                    }
                    else
                    {
                        static_assert(serialise_impl::dependent_false<T>, "Bad integral width in do_serialise");
                    }
                }
                else
                {
                    if constexpr(width == 1)
                    {
                        CHECK_THROW(msgpack_pack_uint8(&ctx.pk, in));
                    }
                    else if constexpr(width == 2)
                    {
                        CHECK_THROW(msgpack_pack_uint16(&ctx.pk, in));
                    }
                    else if constexpr(width == 4)
                    {
                        CHECK_THROW(msgpack_pack_uint32(&ctx.pk, in));
                    }
                    else if constexpr(width == 8)
                    {
                        CHECK_THROW(msgpack_pack_uint64(&ctx.pk, in));
                    }
                    else
                    {
                        static_assert(serialise_impl::dependent_false<T>, "Bad width in do_serialise");
                    }
                }
            }
            else if constexpr(std::is_floating_point_v<T>)
            {
                constexpr int width = sizeof(T);

                if constexpr(width == 4)
                {
                    CHECK_THROW(msgpack_pack_float(&ctx.pk, in));
                }
                else if constexpr(width == 8)
                {
                    CHECK_THROW(msgpack_pack_double(&ctx.pk, in));
                }
                else
                {
                    static_assert(serialise_impl::dependent_false<T>, "Bad float width in do_serialise");
                }
            }
            else if constexpr(std::is_enum_v<T>)
            {
                CHECK_THROW(msgpack_pack_int64(&ctx.pk, (uint64_t)in));
            }
            else if constexpr(std::is_pointer_v<T>)
            {
                uint64_t val = 0;

                uintptr_t as_ptr = (uintptr_t)in;

                if(auto it = ctx.serialising_pointers.find(as_ptr); it != ctx.serialising_pointers.end())
                {
                    val = it->second;

                    CHECK_THROW(msgpack_pack_uint64(&ctx.pk, val));
                }
                else
                {
                    serialise_base(*in, ctx, obj);

                    ctx.serialising_pointers[as_ptr] = ctx.pointer_id++;
                }
            }
            else if constexpr(std::is_same_v<T, std::monostate>)
            {
                CHECK_THROW(msgpack_pack_nil(&ctx.pk));
            }
            else
            {
                static_assert(serialise_impl::dependent_false<T>, "Bad type in do_serialise (encode)");
            }
        }
    }
    else
    {
        if constexpr(std::is_base_of_v<serialise_msgpack, T> || has_serialisable_base<T>())
        {
            serialise_base(in, ctx, obj);
        }
        else
        {
            if constexpr(std::is_integral_v<T>)
            {
                if constexpr(std::is_signed_v<T>)
                {
                    in = obj->via.i64;
                }
                else
                {
                    in = obj->via.u64;
                }
            }
            else if constexpr(std::is_floating_point_v<T>)
            {
                in = obj->via.f64;

                set_unfinite_to_0(in);
            }
            else if constexpr(std::is_enum_v<T>)
            {
                in = (T)obj->via.i64;
            }
            else if constexpr(std::is_pointer_v<T>)
            {
                if(obj->type == MSGPACK_OBJECT_MAP)
                {
                    in = new std::remove_pointer_t<std::decay_t<T>>();

                    ctx.deserialising_pointers[ctx.pointer_id++] = (uintptr_t)in;

                    serialise_base(*in, ctx, obj);
                }
                else
                {
                    uint64_t id = obj->via.u64;

                    auto it = ctx.deserialising_pointers.find(id);

                    assert(it != ctx.deserialising_pointers.end());

                    in = (T)it->second;
                }
            }
            else if constexpr(std::is_same_v<T, std::monostate>)
            {
                in = T();
            }
            else
            {
                static_assert(serialise_impl::dependent_false<T>, "Bad type in do_serialise (decode)");
            }
        }
    }
}

inline
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::string_view& in)
{
    if(ctx.encode)
    {
        CHECK_THROW(msgpack_pack_str(&ctx.pk, in.size()));

        CHECK_THROW(msgpack_pack_str_body(&ctx.pk, in.data(), in.size()));
    }
    else
    {
        ///cannot decode a string_view
        assert(false);
    }
}

inline
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::string& in)
{
    if(ctx.encode)
    {
        CHECK_THROW(msgpack_pack_str(&ctx.pk, in.size()));

        CHECK_THROW(msgpack_pack_str_body(&ctx.pk, in.data(), in.size()));
    }
    else
    {
        in.clear();

        uint32_t len = obj->via.str.size;

        if(obj->via.str.ptr != nullptr)
        {
            in = std::string(obj->via.str.ptr, len);
        }
    }
}

inline
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, const char* in)
{
    if(ctx.encode)
    {
        int ilen = strlen(in);

        CHECK_THROW(msgpack_pack_str(&ctx.pk, ilen));

        CHECK_THROW(msgpack_pack_str_body(&ctx.pk, in, ilen));
    }
    else
    {
        throw std::runtime_error("Cannot deserialise a string");
    }
}

inline
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, nlohmann::json& in)
{
    if(ctx.encode)
    {
        std::vector<uint8_t> dat = nlohmann::json::to_cbor(in);

        if(dat.size() == 0)
        {
            CHECK_THROW(msgpack_pack_nil(&ctx.pk));
        }
        else
        {
            CHECK_THROW(msgpack_pack_bin(&ctx.pk, dat.size()));

            CHECK_THROW(msgpack_pack_bin_body(&ctx.pk, &dat[0], dat.size()));
        }
    }
    else
    {
        if(obj->type == msgpack_object_type::MSGPACK_OBJECT_NIL)
        {
            in = nlohmann::json();
        }
        else
        {
            std::vector<uint8_t> dat(obj->via.bin.ptr, obj->via.bin.ptr + obj->via.bin.size);

            in = nlohmann::json::from_cbor(dat);
        }
    }
}

template<int N, typename T>
inline
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, vec<N, T>& in)
{
    if(ctx.encode)
    {
        CHECK_THROW(msgpack_pack_array(&ctx.pk, N));

        for(int i=0; i < N; i++)
        {
            do_serialise(ctx, nullptr, in.v[i]);
        }
    }
    else
    {
        uint32_t len = obj->via.array.size;

        if(len != (uint32_t)N)
            throw std::runtime_error("Bad array size");

        msgpack_object_array arr = obj->via.array;

        for(int i=0; i < N; i++)
        {
            do_serialise(ctx, &arr.ptr[i], in.v[i]);
        }
    }
}

template<typename T>
inline
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::optional<T>& in)
{
    if(ctx.encode)
    {
        if(!in.has_value())
        {
            msgpack_pack_nil(&ctx.pk);
        }
        else
        {
            do_serialise(ctx, nullptr, in.value());
        }
    }
    else
    {
        if(obj->type == msgpack_object_type::MSGPACK_OBJECT_NIL)
        {
            in = std::nullopt;
        }
        else
        {
            T val = T();

            do_serialise(ctx, obj, val);

            in = val;
        }
    }
}

template<typename T, std::size_t N>
inline
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::array<T, N>& in)
{
    if(ctx.encode)
    {
        CHECK_THROW(msgpack_pack_array(&ctx.pk, N));

        for(int i=0; i < (int)N; i++)
        {
            do_serialise(ctx, nullptr, in[i]);
        }
    }
    else
    {
        in = std::array<T, N>();

        uint32_t len = obj->via.array.size;

        uint32_t min_len = std::min(len, (uint32_t)N);

        for(uint32_t i=0; i < min_len; i++)
        {
            do_serialise(ctx, &obj->via.array.ptr[i], in[i]);
        }
    }
}

template<typename T, typename U>
inline
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::pair<T, U>& in)
{
    if(ctx.encode)
    {
        CHECK_THROW(msgpack_pack_array(&ctx.pk, 2));

        do_serialise(ctx, nullptr, in.first);
        do_serialise(ctx, nullptr, in.second);
    }
    else
    {
        in = std::pair<T, U>();

        uint32_t len = obj->via.array.size;

        uint32_t min_len = std::min(len, (uint32_t)2);

        if(min_len >= 1)
            do_serialise(ctx, &obj->via.array.ptr[0], in.first);

        if(min_len >= 2)
            do_serialise(ctx, &obj->via.array.ptr[1], in.second);
    }
}

template<typename T>
inline
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::vector<T>& in)
{
    if(ctx.encode)
    {
        CHECK_THROW(msgpack_pack_array(&ctx.pk, in.size()));

        for(int i=0; i < (int)in.size(); i++)
        {
            do_serialise(ctx, nullptr, in[i]);
        }
    }
    else
    {
        in.clear();

        uint32_t len = obj->via.array.size;

        in.reserve(len);

        for(uint32_t i=0; i < len; i++)
        {
            T& v = in.emplace_back();

            do_serialise(ctx, &obj->via.array.ptr[i], v);
        }
    }
}

template<typename T>
inline
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::set<T>& in)
{
    if(ctx.encode)
    {
        CHECK_THROW(msgpack_pack_array(&ctx.pk, in.size()));

        for(const T& i : in)
        {
            do_serialise(ctx, nullptr, const_cast<T&>(i));
        }
    }
    else
    {
        in.clear();

        uint32_t len = obj->via.array.size;

        for(uint32_t i=0; i < len; i++)
        {
            T v = T{};

            do_serialise(ctx, &obj->via.array.ptr[i], v);

            in.insert(v);
        }
    }
}

template<typename T, typename U>
inline
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::map<T, U>& in)
{
    if(ctx.encode)
    {
        CHECK_THROW(msgpack_pack_map(&ctx.pk, in.size()));

        for(auto& i : in)
        {
            do_serialise(ctx, nullptr, const_cast<T&>(i.first));
            do_serialise(ctx, nullptr, i.second);
        }
    }
    else
    {
        in.clear();

        uint32_t len = obj->via.map.size;

        for(uint32_t i=0; i < len; i++)
        {
            T key = T();

            do_serialise(ctx, &obj->via.map.ptr[i].key, key);

            do_serialise(ctx, &obj->via.map.ptr[i].val, in[key]);
        }
    }
}

template<typename T, typename U>
inline
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::unordered_map<T, U>& in)
{
    if(ctx.encode)
    {
        CHECK_THROW(msgpack_pack_map(&ctx.pk, in.size()));

        for(auto& i : in)
        {
            do_serialise(ctx, nullptr, const_cast<T&>(i.first));
            do_serialise(ctx, nullptr, i.second);
        }
    }
    else
    {
        in.clear();

        uint32_t len = obj->via.map.size;

        for(uint32_t i=0; i < len; i++)
        {
            T key = T();

            do_serialise(ctx, &obj->via.map.ptr[i].key, key);

            U val = U();

            do_serialise(ctx, &obj->via.map.ptr[i].val, val);

            in[key] = val;
        }
    }
}

namespace detail
{
    template<typename T>
    inline
    void variant_helper2(serialise_context_msgpack& ctx, msgpack_object* obj, T& in, size_t index)
    {

    }

    template<typename T, std::size_t I, std::size_t... I2>
    inline
    void variant_helper2(serialise_context_msgpack& ctx, msgpack_object* obj, T& in, size_t index)
    {
        if(index == I)
        {
            std::variant_alternative_t<I, T> val = std::variant_alternative_t<I, T>();

            do_serialise(ctx, obj, val);

            in = val;
        }

        variant_helper2<T, I2...>(ctx, obj, in, index);
    }

    template<typename T, std::size_t... I>
    inline
    void variant_helper(serialise_context_msgpack& ctx, msgpack_object* obj, T& in, size_t index, std::index_sequence<I...>)
    {
        variant_helper2<T, I...>(ctx, obj, in, index);
    }
}

template<typename... T>
inline
void do_serialise(serialise_context_msgpack& ctx, msgpack_object* obj, std::variant<T...>& in)
{
    if(ctx.encode)
    {
        CHECK_THROW(msgpack_pack_map(&ctx.pk, 1));

        size_t index = in.index();

        do_serialise(ctx, nullptr, index);

        std::visit([&](auto& v)
        {
            do_serialise(ctx, nullptr, v);
        }, in);
    }
    else
    {
        if(obj->via.map.size != 1)
            throw std::runtime_error("Failed to unpack variant");

        size_t index = 0;

        do_serialise(ctx, &obj->via.map.ptr[0].key, index);

        constexpr auto iseq = std::index_sequence_for<T...>{};

        detail::variant_helper(ctx, &obj->via.map.ptr[0].val, in, index, iseq);
    }
}

template<typename T>
inline
std::string serialise_msg(T& in, serialise_context_msgpack& ctx)
{
    ctx.start();
    ctx.encode = true;

    try
    {
        do_serialise(ctx, nullptr, in);

        return std::string(ctx.sbuf.data, ctx.sbuf.size);
    }
    catch(const std::exception& e)
    {
        printf("Error serialising\n");

        std::cout << e.what() << std::endl;
    }

    ctx.stop();

    return "";
}

template<typename T>
inline
std::string serialise_msg(T& in)
{
    serialise_context_msgpack ctx;

    return serialise_msg<T>(in, ctx);
}

template<typename T>
inline
void deserialise_msg(T& ret, std::string_view in, serialise_context_msgpack& ctx)
{
    ctx.start();
    ctx.encode = false;

    msgpack_zone mempool;
    msgpack_zone_init(&mempool, 2048);

    msgpack_object deserialized;
    msgpack_unpack(in.data(), in.size(), NULL, &mempool, &deserialized);

    try
    {
        do_serialise(ctx, &deserialized, ret);
    }
    catch(std::exception& err)
    {
        printf("Error deserialising %s\n", err.what());
    }

    msgpack_zone_destroy(&mempool);

    ctx.stop();
}

template<typename T>
inline
T deserialise_msg(std::string_view in)
{
    T val = T();

    serialise_context_msgpack ctx;

    deserialise_msg(val, in, ctx);

    return val;
}

#endif // SERIALISABLE_MSGPACK_HPP_INCLUDED
