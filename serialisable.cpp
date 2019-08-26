#include "serialisable.hpp"
#include <chrono>
#include <fstream>
#include "netinterpolate.hpp"

/*bool nlohmann_has_name(const nlohmann::json& data, const std::string& name)
{
    return data.count(name) > 0 && !data[name].is_null();
}

bool nlohmann_has_name(const nlohmann::json& data, int name)
{
    return name < data.size() && !data[name].is_null();
}*/

nlohmann::json& nlohmann_index(nlohmann::json& data, const std::string& name)
{
    return data[name];
}

nlohmann::json& nlohmann_index(nlohmann::json& data, int name)
{
    if(data[name].size() < name)
    {
        //data.resize(name);

        if(!data[name].is_array())
            throw std::runtime_error("Expected array");

        while(data[name].size() < name)
            data[name].push_back(nlohmann::json());
    }

    return data[name];
}

/*serialise_context_proxy::serialise_context_proxy(serialise_context& ctx) : last(ctx.data) {}

serialise_context_proxy::serialise_context_proxy(serialise_context_proxy& ctx, const char* name) : last(ctx.last[name]) {}
serialise_context_proxy::serialise_context_proxy(serialise_context_proxy& ctx, int name) : last(ctx.last[name]) {}*/


/*struct test_serialisable : serialisable
{
    SERIALISE_SIGNATURE();

    int test_datamember = 0;
};*/

struct test_serialisable : serialisable, free_function
{
    int test_datamember = 0;
};

DEFINE_SERIALISE_FUNCTION(test_serialisable)
{
    SERIALISE_SETUP();

    DO_FSERIALISE(test_datamember);
}

SERIALISE_BODY(rpc_data)
{
    DO_SERIALISE(id);
    DO_SERIALISE(func);
    DO_SERIALISE(arg);
}

/*void test_serialisable::serialise(serialise_context& ctx, nlohmann::json& data, self_t* other)
{
    DO_SERIALISE(test_datamember);
}*/



SERIALISE_BODY(global_serialise_info)
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

size_t serialisable_time_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct data_1 : serialisable, owned
{
    float my_float = 0;

    SERIALISE_SIGNATURE(data_1)
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

    SERIALISE_SIGNATURE(data_2)
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

    nlohmann::json ser_data = serialise_against(dat_3, dat_2, false, 0);

    data_2 mdata = dat_2;

    deserialise(ser_data, mdata);

    assert(mdata.test_owned.size() == 2);
    assert(mdata.test_owned[0].my_float == 2);
    assert(mdata.test_owned[1].my_float == 53);


    {
        data_1 test_data;
        test_data.my_float = NAN;

        auto dat = serialise(test_data);

        data_1 receive = deserialise<data_1>(dat);

        assert(receive.my_float == 0);
    }
}

global_serialise_info& get_global_serialise_info()
{
    thread_local static global_serialise_info inf;

    return inf;
}

void default_callback(size_t current, size_t requested, void* udata)
{

}

pid_callback_t& get_current_callback()
{
    thread_local static pid_callback_t call = default_callback;

    return call;
}

void set_pid_callback(pid_callback_t callback)
{
    get_current_callback() = callback;
}

void*& get_pid_udata()
{
    thread_local static void* udata = nullptr;

    return udata;
}

void set_pid_udata(void* udata)
{
    get_pid_udata() = udata;
}

size_t& get_id_cap()
{
    thread_local static size_t gid = 0;

    return gid;
}

size_t& get_raw_id_impl()
{
    thread_local static size_t gpid = 0;

    return gpid;
}

size_t get_next_persistent_id()
{
    size_t& gpid = get_raw_id_impl();

    assert(gpid <= get_id_cap());

    if(gpid == get_id_cap())
    {
        get_current_callback()(gpid, 1024, get_pid_udata());
        get_id_cap() += 1024;
    }

    return gpid++;
}

void set_next_persistent_id(size_t in)
{
    get_raw_id_impl() = in;
    get_id_cap() = in;
}

void save_to_file(const std::string& fname, const nlohmann::json& data)
{
    std::vector<unsigned char> input = nlohmann::json::to_cbor(data);
    std::ofstream out(fname, std::ios::binary);
    out << std::string(input.begin(), input.end());
}

nlohmann::json load_from_file(const std::string& fname)
{
    std::ifstream t(fname, std::ios::binary);
    std::string str((std::istreambuf_iterator<char>(t)),
                     std::istreambuf_iterator<char>());

    return nlohmann::json::from_cbor(str);
}

void save_to_file_json(const std::string& fname, const nlohmann::json& data)
{
    std::ofstream out(fname, std::ios::binary);
    out << data.dump();
}

nlohmann::json load_from_file_json(const std::string& fname)
{
    std::ifstream t(fname, std::ios::binary);
    std::string str((std::istreambuf_iterator<char>(t)),
                     std::istreambuf_iterator<char>());

    return nlohmann::json::parse(str);
}
