#include "serialisable.hpp"
#include <chrono>

void test_serialisable::serialise(serialise_context& ctx, nlohmann::json& data, bool encode)
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
