#ifndef SERIALISABLE_MSGPACK_HPP_INCLUDED
#define SERIALISABLE_MSGPACK_HPP_INCLUDED

#include <msgpack.h>
#include <cstdint>
#include "serialisable.hpp"

/*#define DO_MSG_FSERIALISE_BASE(obj, x)  std::string_view my_name = #x;\
                                        if(ctx.serialisation) \
                                        { \
                                            \
                                        }


#define DO_MSG_FSERAILISE(x) DO_MSG_FSERIALISE_BASE(me, x)*/

#define DECLARE_MSG_FSERIALISE(x) void serialise_base(x& me, serialise_context& ctx, msgpack_object* obj)

#define SETUP_MSG_FSERIALISE(cnt)  CHECK_THROW(msgpack_pack_map(&ctx.pk, cnt));

#define DO_MSG_FSERIALISE(x, id, name) touch_member_base(ctx, obj, me.x, id, name)
#define DO_MSG_FSERIALISE_SIMPLE(x, id) touch_member_base(ctx, obj, me.x, id, #x)

#define CHECK_THROW(x) do{if(auto rval = (x); rval != 0) { throw std::runtime_error("Serialisation failed " + std::to_string(rval)); } } while(0)

struct serialise_msgpack
{

};

void do_serialise(serialise_context& ctx, msgpack_object* obj, std::string& in);
void do_serialise(serialise_context& ctx, msgpack_object* obj, const char* in);

template<typename T, typename name_type>
inline
void touch_member_base(serialise_context& ctx, msgpack_object* obj, T& in, int id, name_type name)
{
    if(ctx.encode)
    {
        do_serialise(ctx, nullptr, name);
        do_serialise(ctx, nullptr, in);
    }
    else
    {
        in = T();

        if(id < (int)obj->via.map.size)
        {
            uint32_t len = obj->via.map.ptr[id].key.via.str.size;

            if(strncmp(obj->via.map.ptr[id].key.via.str.ptr, name, len) == 0)
            {
                do_serialise(ctx, &obj->via.map.ptr[id].val, in);

                return;
            }
        }

        for(int i=0; i < (int)obj->via.map.size; i++)
        {
            uint32_t len = obj->via.map.ptr[id].key.via.str.size;

            if(strncmp(obj->via.map.ptr[i].key.via.str.ptr, name, len) == 0)
            {
                do_serialise(ctx, &obj->via.map.ptr[i].val, in);

                return;
            }
        }
    }
}

template<typename T>
inline
void do_serialise(serialise_context& ctx, msgpack_object* obj, T& in)
{
    if(ctx.encode)
    {
        if constexpr(std::is_base_of_v<serialise_msgpack, T>)
        {
            serialise_base(in, ctx, obj);
        }

        else if constexpr(std::is_same_v<bool, T>)
        {
            int8_t val = in;

            CHECK_THROW(msgpack_pack_signed_char(&ctx.pk, val));
        }

        else if constexpr(std::is_same_v<std::int8_t, T>)
        {
            CHECK_THROW(msgpack_pack_signed_char(&ctx.pk, in));
        }

        else if constexpr(std::is_same_v<std::uint8_t, T>)
        {
            CHECK_THROW(msgpack_pack_unsigned_char(&ctx.pk, in));
        }

        else if constexpr(std::is_same_v<std::int16_t, T>)
        {
            CHECK_THROW(msgpack_pack_int16(&ctx.pk, in));
        }

        else if constexpr(std::is_same_v<std::uint16_t, T>)
        {
            CHECK_THROW(msgpack_pack_uint16(&ctx.pk, in));
        }

        else if constexpr(std::is_same_v<std::int32_t, T>)
        {
            CHECK_THROW(msgpack_pack_int32(&ctx.pk, in));
        }

        else if constexpr(std::is_same_v<std::uint32_t, T>)
        {
            CHECK_THROW(msgpack_pack_uint32(&ctx.pk, in));
        }

        else if constexpr(std::is_same_v<std::int64_t, T>)
        {
            CHECK_THROW(msgpack_pack_int64(&ctx.pk, in));
        }

        else if constexpr(std::is_same_v<std::uint64_t, T>)
        {
            CHECK_THROW(msgpack_pack_uint64(&ctx.pk, in));
        }

        else if constexpr(std::is_same_v<float, T>)
        {
            CHECK_THROW(msgpack_pack_float(&ctx.pk, in));
        }

        else if constexpr(std::is_same_v<double, T>)
        {
            CHECK_THROW(msgpack_pack_double(&ctx.pk, in));
        }

        else
        {
            throw std::runtime_error("well that's a mistake");
        }
    }
    else
    {
        if constexpr(std::is_base_of_v<serialise_msgpack, T>)
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
            else
            {
                in = obj->via.f64;
            }
        }

    }
}

inline
void do_serialise(serialise_context& ctx, msgpack_object* obj, std::string& in)
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
void do_serialise(serialise_context& ctx, msgpack_object* obj, const char* in)
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

template<int N, typename T>
inline
void do_serialise(serialise_context& ctx, msgpack_object* obj, vec<N, T>& in)
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

        msgpack_object_array* arr = obj->via.array.ptr;

        for(int i=0; i < N; i++)
        {
            do_serialise(ctx, arr->ptr[i], in.v[i]);
        }
    }
}

template<int N, typename T>
inline
void do_serialise(serialise_context& ctx, msgpack_object* obj, std::optional<T>& in)
{
    if(ctx.encode)
    {
        if(!in.has_value())
            return;

        do_serialise(ctx, nullptr, in.value());
    }
    else
    {
        in = std::nullopt;

        T val = T();

        do_serialise(ctx, obj, val);

        in = val;
    }
}

template<typename T, std::size_t N>
inline
void do_serialise(serialise_context& ctx, msgpack_object* obj, std::array<T, N>& in)
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
        uint32_t len = obj->via.array.size;

        uint32_t min_len = std::min(len, (uint32_t)N);

        for(int i=0; i < (int)min_len; i++)
        {
            do_serialise(ctx, obj->via.array.ptr[i], in[i]);
        }
    }
}

template<typename T>
inline
void do_serialise(serialise_context& ctx, msgpack_object* obj, std::vector<T>& in)
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

        for(uint32_t i=0; i < len; i++)
        {
            T& v = in.emplace_back();

            do_serialise(ctx, &obj->via.array.ptr[i], v);
        }
    }
}

template<typename T, typename U>
inline
void do_serialise(serialise_context& ctx, msgpack_object* obj, std::map<T, U>& in)
{
    if(ctx.encode)
    {
        CHECK_THROW(msgpack_pack_map(&ctx.pk, in.size()));

        for(auto& i : in)
        {
            do_serialise(ctx, nullptr, i.first);
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

            do_serialise(ctx, obj->via.map.ptr[i].key, key);

            U val = U();

            do_serialise(ctx, obj->via.map.ptr[i].val, val);

            in[key] = val;
        }
    }
}

template<typename T>
inline
std::string serialise_msg(T& in)
{
    serialise_context ctx;
    ctx.encode = true;
    ctx.serialisation = true;
    ctx.mode = serialise_mode::DISK;

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

    return "";
}

template<typename T>
inline
T deserialise_msg(const std::string& in)
{
    serialise_context ctx;
    ctx.encode = false;
    ctx.serialisation = true;
    ctx.mode = serialise_mode::DISK;

    msgpack_zone mempool;
    msgpack_zone_init(&mempool, 2048);

    msgpack_object deserialized;
    msgpack_unpack(in.c_str(), in.size(), NULL, &mempool, &deserialized);

    T ret = T();

    try
    {
        do_serialise(ctx, &deserialized, ret);
    }
    catch(...)
    {
        printf("Error deserialising\n");
    }

    msgpack_zone_destroy(&mempool);

    return ret;
}

#endif // SERIALISABLE_MSGPACK_HPP_INCLUDED
