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

    if(ctx.serialisation && ctx.encode == false)
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

struct data_1 : serialisable, owned
{
    float my_float = 0;

    SERIALISE_SIGNATURE()
    {
        DO_SERIALISE(my_float);
    }
};

struct data_2 : serialisable, owned
{
    std::vector<data_1> test_owned;

    data_2()
    {
        data_1 mdata;
        mdata.my_float = 2;

        test_owned.push_back(mdata);
    }

    SERIALISE_SIGNATURE()
    {
        DO_SERIALISE(test_owned);
    }
};

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



    data_2 dat_2;
    data_2 dat_3;

    data_1 add_data;
    add_data.my_float = 53;

    dat_3.test_owned.push_back(add_data);

    nlohmann::json ser_data = serialise_against(dat_3, dat_2);

    data_2 mdata = dat_2;

    deserialise(ser_data, mdata);

    assert(mdata.test_owned.size() == 2);
    assert(mdata.test_owned[0].my_float == 2);
    assert(mdata.test_owned[1].my_float == 53);
}
