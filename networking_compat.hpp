#ifndef NETWORKING_COMPAT_HPP_INCLUDED
#define NETWORKING_COMPAT_HPP_INCLUDED

#define USE_STEAM_NETWORKING
#ifdef USE_STEAM_NETWORKING
#include "networking_steam.hpp"
using namespace networking2;
#else
#include "networking.hpp"
#endif

#endif // NETWORKING_COMPAT_HPP_INCLUDED
