#ifndef SERIALISABLE_MSGPACK_FWD_HPP_INCLUDED
#define SERIALISABLE_MSGPACK_FWD_HPP_INCLUDED

struct serialise_context_msgpack;
struct msgpack_object;

#define DECLARE_MSG_FSERIALISE(x) void serialise_base(x& me, serialise_context_msgpack& ctx, msgpack_object* obj)
#define DEFINE_MSG_FSERIALISE(x) void serialise_base(x& me, serialise_context_msgpack& ctx, msgpack_object* obj)

struct serialise_msgpack{};

#endif // SERIALISABLE_MSGPACK_FWD_HPP_INCLUDED
