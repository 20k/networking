#include "serialisable.hpp"
#include <chrono>

void rpc_data::serialise(serialise_context& ctx, nlohmann::json& data, self_t* other)
{
    DO_SERIALISE(id);
    DO_SERIALISE(func);
    DO_SERIALISE(arg);
}

void test_serialisable::serialise(serialise_context& ctx, nlohmann::json& data, self_t* other)
{
    DO_SERIALISE(test_datamember);
}

void global_serialise_info::serialise(serialise_context& ctx, nlohmann::json& data, self_t* other)
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

size_t serialisable::time_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

serialisable::~serialisable()
{

}

void serialise_tests()
{
    float f1 = 1;
    float f2 = 1;

    assert(serialisable_is_equal(&f1, &f2));

    float f3 = 2;

    assert(!serialisable_is_equal(&f1, &f3));

    test_serialisable test;
    test.test_datamember = 0;

    test_serialisable test2;
    test2.test_datamember = 1;

    assert(!serialisable_is_equal(&test, &test2));

    test_serialisable test3;
    test3.test_datamember = 0;

    assert(serialisable_is_equal(&test, &test3));


    std::vector<test_serialisable> v_1;
    std::vector<test_serialisable> v_2;

    for(int i=0; i < 5; i++)
    {
        v_1.push_back(test);
        v_2.push_back(test);
    }

    assert(serialisable_is_equal(&v_1, &v_2));

    v_2.push_back(test2);

    assert(!serialisable_is_equal(&v_1, &v_2));
}
