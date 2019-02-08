#include "serialisable.hpp"

void test_serialisable::serialise(nlohmann::json& data, bool encode)
{
    DO_SERIALISE(test_datamember);
}
