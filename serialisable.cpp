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

size_t serialisable::time_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

serialisable::~serialisable()
{

}
