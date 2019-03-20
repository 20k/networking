#ifndef NETINTERPOLATE_HPP_INCLUDED
#define NETINTERPOLATE_HPP_INCLUDED

#include <vector>
#include "serialisable.hpp"
#include <atomic>
#include <assert.h>

template<typename T, bool auth>
struct net_interpolate : serialisable
{
    T host = T();

    static inline std::atomic_int global_ids{0};

    int id = global_ids++;

    ///received from authoratitive client
    std::vector<T> values;
    ///timestamps at which those values were received, assumes clock sync
    std::vector<size_t> timestamps;

    SERIALISE_SIGNATURE()
    {
        if(!ctx.serialisation)
            return;

        ///will probably need this for save/load
        if(auth && !encode)
        {
            std::cout << "fuckup\n";
            return;
        }

        if(auth && encode)
        {
            data["_i"] = id;
            do_serialise(data, host, "_r" + std::to_string(id), encode);

            size_t server_time = serialisable::time_ms();
            do_serialise(data, server_time, "_t" + std::to_string(id), true);
        }

        if(!auth && !encode)
        {
            id = data["_i"];

            T temp = T();
            do_serialise(data, temp, "_r" + std::to_string(id), encode);

            size_t server_time = 0;
            do_serialise(data, server_time, "_t" + std::to_string(id), false);

            std::cout << "stime " << server_time << std::endl;

            if(values.size() > 2)
            {
                values.erase(values.begin());
                timestamps.erase(timestamps.begin());
            }

            values.push_back(temp);
            timestamps.push_back(server_time);
        }

        if(!auth && encode)
        {
            std::cout << "fuckup pt 2\n";
            return;
        }
    }

    T get_interpolated(size_t epoch_time_ms)
    {
        if(auth)
            return host;

        if(values.size() == 0)
            return T();

        assert(values.size() == timestamps.size());

        for(int i=0; i < (int)values.size(); i++)
        {
            if(i == (int)values.size() - 1)
                return values[i];

            int next = i + 1;

            assert(next < (int)values.size());

            std::cout << "epoch\n";

            if(epoch_time_ms >= timestamps[i] && epoch_time_ms < timestamps[next])
            {
                size_t diff = timestamps[next] - timestamps[i];

                size_t offset = epoch_time_ms - timestamps[i];

                double fraction = (double)offset / diff;

                std::cout << "HELLO\n";

                return values[i] * (1. - fraction) + values[next] * fraction;
            }
        }

        return values[0];
    }

    void set_host(const T& val)
    {
        host = val;
    }
};

#endif // NETINTERPOLATE_HPP_INCLUDED
