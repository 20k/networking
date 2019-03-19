#include "serialisable.hpp"
#include <chrono>

void rpc_data::serialise(serialise_context& ctx, nlohmann::json& data)
{
    DO_SERIALISE(id);
    DO_SERIALISE(func);
    DO_SERIALISE(arg);
}

void test_serialisable::serialise(serialise_context& ctx, nlohmann::json& data)
{
    DO_SERIALISE(test_datamember);
}

void global_serialise_info::serialise(serialise_context& ctx, nlohmann::json& data)
{
    DO_SERIALISE(all_rpcs);

    if(ctx.encode == false)
    {
        built.clear();

        for(auto& i : all_rpcs)
        {
            built[i.id].push_back(i);
        }
    }
}

///need to integrate network ownership in here
void global_serialise_info::consume(std::map<size_t, serialisable*>& in)
{
    for(rpc_data& dat : all_rpcs)
    {
        auto it = in.find(dat.id);

        if(it == in.end())
            continue;

        serialisable* s = it->second;

        s->execute_function(dat.func, dat.arg);
    }

    all_rpcs.clear();
}

size_t serialisable::time_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

serialisable::~serialisable()
{

}
