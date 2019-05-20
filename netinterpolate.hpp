#ifndef NETINTERPOLATE_HPP_INCLUDED
#define NETINTERPOLATE_HPP_INCLUDED

#include <vector>
#include <assert.h>
#include <any>

/*template<typename T>
struct smoothed : serialisable
{
    T real = T();

    std::vector<T> values;
    std::vector<size_t> timestamps;

    smoothed(const smoothed<T>& in)
    {
        real = in.real;
        values = in.values;
        timestamps = in.timestamps;
    }

    smoothed(const T& in)
    {
        real = in;
    }

    SERIALISE_SIGNATURE()
    {
        DO_SERIALISE(real);

        if(ctx.serialisation && !ctx.encode)
        {
            size_t stamp = serialisable::time_ms();

            add_val(real, stamp);
        }
    }

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

    T get_update()
    {
        ///200 should be server ping estimate
        size_t epoch_time_ms = serialisable::time_ms() - 200;

        if(values.size() == 0)
            return T();

        assert(values.size() == timestamps.size());

        for(int i=0; i < (int)values.size(); i++)
        {
            if(i == (int)values.size() - 1)
                return values[i];

            int next = i + 1;

            assert(next < (int)values.size());

            if(epoch_time_ms >= timestamps[i] && epoch_time_ms < timestamps[next])
            {
                size_t diff = timestamps[next] - timestamps[i];

                size_t offset = epoch_time_ms - timestamps[i];

                double fraction = (double)offset / diff;

                return values[i] * (1. - fraction) + values[next] * fraction;
            }
        }

        return values[0];
    }

    void update()
    {
        real = get_update();
    }

    operator const T&() const
    {
        return real;
    }

    operator T&()
    {
        return real;
    }

    smoothed<T>& operator=(const smoothed<T>& other) noexcept
    {
        real = other.real;
        values = other.values;
        timestamps = other.timestamps;

        return *this;
    }

    smoothed<T>& operator=(const T& other) noexcept
    {
        real = other;

        return real;
    }

    smoothed<T>& operator += (const T& rhs)
    {
        real += rhs;
        return *this;
    }

    friend smoothed<T> operator+(smoothed<T> lhs, const T& rhs)
    {
        lhs.real += rhs;
        return lhs;
    }

    smoothed<T>& operator -= (const T& rhs)
    {
        real -= rhs;
        return *this;
    }

    friend smoothed<T> operator-(smoothed<T> lhs, const T& rhs)
    {
        lhs.real -= rhs;
        return lhs;
    }

    smoothed<T>& operator *= (const T& rhs)
    {
        real *= rhs;
        return *this;
    }

    friend smoothed<T> operator*(smoothed<T> lhs, const T& rhs)
    {
        lhs.real *= rhs;
        return lhs;
    }

    smoothed<T>& operator /= (const T& rhs)
    {
        real /= rhs;
        return *this;
    }

    friend smoothed<T> operator/(smoothed<T> lhs, const T& rhs)
    {
        lhs.real /= rhs;
        return lhs;
    }

    friend bool operator<(const smoothed<T>& lhs, const smoothed<T>& rhs)
    {
        return lhs.real < rhs.real;
    }

    friend bool operator<(const smoothed<T>& lhs, const T& rhs)
    {
        return lhs.real < rhs;
    }

    friend bool operator> (const smoothed<T>& lhs, const T& rhs){ return rhs < lhs; }
    friend bool operator> (const smoothed<T>& lhs, const smoothed<T>& rhs){ return rhs < lhs; }

    friend bool operator<=(const smoothed<T>& lhs, const T& rhs){ return !(rhs > lhs); }
    friend bool operator<=(const smoothed<T>& lhs, const smoothed<T>& rhs){ return !(rhs > lhs); }

    friend bool operator>=(const smoothed<T>& lhs, const T& rhs){ return !(rhs < lhs); }
    friend bool operator>=(const smoothed<T>& lhs, const smoothed<T>& rhs){ return !(rhs < lhs); }

    friend bool operator==(const smoothed<T>& lhs, const smoothed<T>& rhs){return lhs.real == rhs.real;}
    friend bool operator==(const smoothed<T>& lhs, const T& rhs){return lhs.real == rhs;}

    friend bool operator!=(const smoothed<T>& lhs, const smoothed<T>& rhs){return !(lhs == rhs);}
    friend bool operator!=(const smoothed<T>& lhs, const T& rhs){return !(lhs == rhs);}
};*/

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

/*template<typename T, bool auth>
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
};*/

#endif // NETINTERPOLATE_HPP_INCLUDED
