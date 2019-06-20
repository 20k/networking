#ifndef NETINTERPOLATE_HPP_INCLUDED
#define NETINTERPOLATE_HPP_INCLUDED

#include <vector>
#include <assert.h>
#include <any>

size_t serialisable_time_ms();

struct ts_vector
{
    std::vector<std::any> values;
    std::vector<size_t> timestamps;

    template<typename T>
    void add_val(const T& val, size_t stamp)
    {
        ///make this time dependent instead
        while(values.size() > 10)
        {
            values.erase(values.begin());
            timestamps.erase(timestamps.begin());
        }

        values.push_back(val);
        timestamps.push_back(stamp);
    }

    template<typename T>
    T get_update()
    {
        ///200 should be server ping estimate
        size_t epoch_time_ms = serialisable_time_ms() - 1000;

        if(values.size() == 0)
            return T();

        assert(values.size() == timestamps.size());

        for(int i=0; i < (int)values.size(); i++)
        {
            if(i == (int)values.size() - 1)
                return std::any_cast<T>(values[i]);

            int next = i + 1;

            assert(next < (int)values.size());

            if(epoch_time_ms >= timestamps[i] && epoch_time_ms < timestamps[next])
            {
                size_t diff = timestamps[next] - timestamps[i];

                size_t offset = epoch_time_ms - timestamps[i];

                double fraction = (double)offset / diff;

                return std::any_cast<T>(values[i]) * (1. - fraction) + std::any_cast<T>(values[next]) * fraction;
            }
        }

        return std::any_cast<T>(values[0]);
    }
};

#endif // NETINTERPOLATE_HPP_INCLUDED
