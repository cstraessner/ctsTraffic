/*

Copyright (c) Microsoft Corporation
All rights reserved.

Licensed under the Apache License, Version 2.0 (the ""License""); you may not use this file except in compliance with the License. You may obtain a copy of the License at http://www.apache.org/licenses/LICENSE-2.0

THIS CODE IS PROVIDED ON AN  *AS IS* BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING WITHOUT LIMITATION ANY IMPLIED WARRANTIES OR CONDITIONS OF TITLE, FITNESS FOR A PARTICULAR PURPOSE, MERCHANTABLITY OR NON-INFRINGEMENT.

See the Apache Version 2.0 License for specific language governing permissions and limitations under the License.

*/

// cpp headers
// ReSharper disable CppClangTidyCppcoreguidelinesInterfacesGlobalInit
// ReSharper disable CppClangTidyClangDiagnosticExitTimeDestructors
#include <vector>
#include <string>
#include <algorithm>
// os headers
#include <Windows.h>
#include <WinSock2.h>
#include <mstcpip.h>
#include <iphlpapi.h>
// multimedia timer
#include <mmsystem.h>
// wil headers
#include <wil/stl.h>
#include <wil/resource.h>
// ctl headers
#include <ctSockaddr.hpp>
#include <ctString.hpp>
#include <ctNetAdapterAddresses.hpp>
#include <ctSocketExtensions.hpp>
#include <ctTimer.hpp>
#include <ctRandom.hpp>
#include <ctWmiInitialize.hpp>
// project headers
#include "ctsConfig.h"
#include "ctsLogger.hpp"
#include "ctsIOPattern.h"
#include "ctsPrintStatus.hpp"
// project functors
#include "ctsTCPFunctions.h"
#include "ctsMediaStreamClient.h"
#include "ctsMediaStreamServer.h"
#include "ctsWinsockLayer.h"

using namespace std;
using namespace ctl;

namespace ctsTraffic::ctsConfig
{
////////////////////////////////////////////////////////////////////////////////////////////////////
///
/// Settings is being defined in this cpp - it was extern'd from ctsConfig.h
///
////////////////////////////////////////////////////////////////////////////////////////////////////
ctsConfigSettings* g_configSettings;

////////////////////////////////////////////////////////////////////////////////////////////////////
///
/// Hiding the details of the raw data in an unnamed namespace to make it completely private
/// Free functions below provide proper access to this information
/// This design avoids having to pass a "config" object all over to share this information
///
////////////////////////////////////////////////////////////////////////////////////////////////////
static wil::critical_section g_statusUpdateLock{ctsConfigSettings::c_CriticalSectionSpinlock};
static wil::critical_section g_shutdownLock{ctsConfigSettings::c_CriticalSectionSpinlock};

constexpr WORD c_defaultPort = 4444;

constexpr uint64_t c_defaultTransfer = 0x40000000; // 1Gbyte

constexpr uint32_t c_defaultBufferSize = 0x10000; // 64kbyte
constexpr uint32_t c_defaultAcceptLimit = 10;
constexpr uint32_t c_defaultAcceptExLimit = 100;
constexpr uint32_t c_defaultTcpConnectionLimit = 8;
constexpr uint32_t c_defaultUdpConnectionLimit = 1;
constexpr uint32_t c_defaultConnectionThrottleLimit = 1000;
constexpr uint32_t c_defaultThreadpoolFactor = 2;

static PTP_POOL g_threadPool = nullptr;
static TP_CALLBACK_ENVIRON g_threadPoolEnvironment;
static uint32_t g_threadPoolThreadCount = 0;

static const wchar_t* g_createFunctionName = nullptr;
static const wchar_t* g_connectFunctionName = nullptr;
static const wchar_t* g_acceptFunctionName = nullptr;
static const wchar_t* g_ioFunctionName = nullptr;

// connection info + error info
static uint32_t g_consoleVerbosity = 4;
static uint32_t g_bufferSizeLow = 0;
static uint32_t g_bufferSizeHigh = 0;
static int64_t g_rateLimitLow = 0;
static int64_t g_rateLimitHigh = 0;
static uint64_t g_transferSizeLow = c_defaultTransfer;
static uint64_t g_transferSizeHigh = 0;

constexpr uint32_t c_defaultPushBytes = 0x100000;
constexpr uint32_t c_defaultPullBytes = 0x100000;

static uint32_t g_timePeriodRefCount{};

static int64_t g_previousPrintTimeslice{};
static int64_t g_printTimesliceCount{};

static NET_IF_COMPARTMENT_ID g_compartmentId = NET_IF_COMPARTMENT_ID_UNSPECIFIED;
static ctNetAdapterAddresses* g_netAdapterAddresses = nullptr;

static MediaStreamSettings g_mediaStreamSettings;
static ctRandomTwister g_randomTwister;

// default to 5 seconds
constexpr uint32_t c_defaultStatusUpdateFrequency = 5000;
static shared_ptr<ctsStatusInformation> g_printStatusInformation;
static shared_ptr<ctsLogger> g_connectionLogger;
static shared_ptr<ctsLogger> g_statusLogger;
static shared_ptr<ctsLogger> g_errorLogger;
static shared_ptr<ctsLogger> g_jitterLogger;
static shared_ptr<ctsLogger> g_tcpInfoLogger;

static bool g_breakOnError = false;
static bool g_shutdownCalled = false;


////////////////////////////////////////////////////////////////////////////////////////////////////////////////
///
/// Singleton values used as the actual implementation for every 'connection'
///
/// publicly exposed callers invoke ::InitOnceExecuteOnce(&InitImpl, InitOncectsConfigImpl, NULL, NULL);
///
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
static INIT_ONCE g_configInitImpl = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK InitOncectsConfigImpl(PINIT_ONCE, PVOID, PVOID*)
{
    g_configSettings = new ctsConfigSettings;
    g_configSettings->Port = c_defaultPort;
    WI_SetAllFlags(g_configSettings->SocketFlags, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    g_configSettings->Iterations = MAXULONGLONG;
    g_configSettings->ConnectionLimit = 1;
    g_configSettings->AcceptLimit = c_defaultAcceptLimit;
    g_configSettings->ConnectionThrottleLimit = c_defaultConnectionThrottleLimit;
    g_configSettings->ServerExitLimit = MAXULONGLONG;
    g_configSettings->StatusUpdateFrequencyMilliseconds = c_defaultStatusUpdateFrequency;
    // defaulting to verifying - therefore not using a shared buffer
    g_configSettings->ShouldVerifyBuffers = true;
    g_configSettings->UseSharedBuffer = false;

    g_previousPrintTimeslice = 0LL;
    g_printTimesliceCount = 0LL;

    return TRUE;
}

static void ctsConfigInitOnce() noexcept
{
    FAIL_FAST_IF(!InitOnceExecuteOnce(&g_configInitImpl, InitOncectsConfigImpl, nullptr, nullptr));
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// parses the configuration of the local system for options dependent on deployments
///
//////////////////////////////////////////////////////////////////////////////////////////
static void CheckSystemSettings() noexcept try
{
    // Windows 10+ exposes a new socket option: SO_REUSE_UNICASTPORT
    // - this allows for much greater reuse of local ports, but also requires
    //   the system having been deliberately configured to take advantege of it
    // - looking for corresponding the WMI class property, which only exists in Win10+
    const auto com = wil::CoInitializeEx();
    const ctWmiService wmiService(L"ROOT\\StandardCimv2");

    ctWmiEnumerate tcpSettings(wmiService);
    tcpSettings.query(L"SELECT * FROM MSFT_NetTCPSetting");
    for (const auto& instance : tcpSettings)
    {
        // ctl::ctWmiInstance& instance
        wil::unique_variant varValue;
        instance.get(L"AutoReusePortRangeNumberOfPorts", varValue.addressof());
        if (V_VT(varValue.addressof()) == VT_I4)
        {
            if (V_I4(varValue.addressof()) != 0)
            {
                g_configSettings->Options |= ReuseUnicastPort;
            }
        }
    }
}
catch (...)
{
    // will assume is not configured if any exception is thrown
    // - could be the class doesn't exist (Win7)
    //   or the property doesn't exist (Win8 and 8.1)
    PRINT_DEBUG_INFO(L"Not using SO_REUSE_UNICASTPORT as AutoReusePortRangeNumberOfPorts is not supported or not configured");
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// parses the input argument to determine if it matches the expected parameter
/// if so, it returns a ptr to the corresponding parameter value
/// otherwise, returns nullptr
///
/// throws invalid_parameter if something is obviously wrong 
///
//////////////////////////////////////////////////////////////////////////////////////////
static const wchar_t* ParseArgument(_In_z_ const wchar_t* inputArgument, _In_z_ const wchar_t* expectedParam)
{
    const wchar_t* paramEnd = inputArgument + wcslen(inputArgument);
    const wchar_t* paramDelimeter = find(inputArgument, paramEnd, L':');
    if (!(paramEnd > paramDelimeter + 1))
    {
        throw invalid_argument(ctString::ctConvertToString(inputArgument));
    }
    // temporarily null-terminate it at the delimiter to do a string compare
    *const_cast<wchar_t*>(paramDelimeter) = L'\0';
    const wchar_t* returnValue = nullptr;
    if (ctString::ctOrdinalEqualsCaseInsensative(expectedParam, inputArgument))
    {
        returnValue = paramDelimeter + 1;
    }
    *const_cast<wchar_t*>(paramDelimeter) = L':';
    return returnValue;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
///
/// ConvertToIntegral(wstring)
///
/// Directly converts the *entire* contents of the passed in string to a numeric value
/// - the type of that numeric value being the template type specified
///
/// e.g.
/// long a = ConvertToIntegral<long>(L"-1");
/// long b = ConvertToIntegral<uint32_t>(L"0xa");
/// long a = ConvertToIntegral<int64_t>(L"0x123456789abcdef");
/// long a = ConvertToIntegral<uint64_t>(L"999999999999999999");
/// 
/// NOTE:
/// - will *only* assume a string starting with "0x" to be converted as hexadecimal
///   if does not start with "0x", will assume as base-10
/// - if an unsigned type is specified in the template and a negative number is entered,
///   will convert that to the "unsigned" version of that set of bits
///   e.g.
///       uint64_t test = ConvertToIntegral<uint64_t>(L"-1");
///       // test == 0xffffffffffffffff
///
////////////////////////////////////////////////////////////////////////////////////////////////////
template <typename T>
T ConvertToIntegral(const wstring&)
{
    // ReSharper disable once CppStaticAssertFailure
    static_assert(false, "Only supports the below specializations");
    return {};
}

// LONG and ULONG
template <>
long ConvertToIntegral<long>(const wstring& inputString)
{
    auto returnValue = 0l;
    size_t firstUnconvertedOffset = 0;
    if (inputString.find(L'x') != wstring::npos || inputString.find(L'X') != wstring::npos)
    {
        returnValue = stol(inputString, &firstUnconvertedOffset, 16);
    }
    else
    {
        returnValue = stol(inputString, &firstUnconvertedOffset, 10);
    }

    if (firstUnconvertedOffset != inputString.length())
    {
        throw invalid_argument(ctString::ctConvertToString(inputString));
    }
    return returnValue;
}

template <>
unsigned long ConvertToIntegral<unsigned long>(const wstring& inputString)
{
    auto returnValue = 0ul;
    size_t firstUnconvertedOffset = 0;
    if (inputString.find(L'x') != wstring::npos || inputString.find(L'X') != wstring::npos)
    {
        returnValue = stoul(inputString, &firstUnconvertedOffset, 16);
    }
    else
    {
        returnValue = stoul(inputString, &firstUnconvertedOffset, 10);
    }

    if (firstUnconvertedOffset != inputString.length())
    {
        throw invalid_argument(ctString::ctConvertToString(inputString));
    }
    return returnValue;
}

// INT and UINT
template <>
int ConvertToIntegral<int>(const wstring& inputString)
{
    return ConvertToIntegral<long>(inputString);
}

template <>
unsigned int ConvertToIntegral<unsigned int>(const wstring& inputString)
{
    return ConvertToIntegral<unsigned long>(inputString);
}

// SHORT and USHORT
template <>
short ConvertToIntegral<short>(const wstring& inputString)
{
    const long returnValue = ConvertToIntegral<long>(inputString);
    if (returnValue > MAXSHORT || returnValue < MINSHORT)
    {
        throw invalid_argument(ctString::ctConvertToString(inputString));
    }
    return static_cast<short>(returnValue);
}

template <>
unsigned short ConvertToIntegral<unsigned short>(const wstring& inputString)
{
    const uint32_t returnValue = ConvertToIntegral<uint32_t>(inputString);
    // MAXWORD == MAXUSHORT
    if (returnValue > MAXWORD)
    {
        throw invalid_argument(ctString::ctConvertToString(inputString));
    }
    return static_cast<unsigned short>(returnValue);
}

// LONGLONG and ULONGLONG
template <>
int64_t ConvertToIntegral<int64_t>(const wstring& inputString)
{
    auto returnValue = 0ll;
    size_t firstUnconvertedOffset = 0;
    if (inputString.find(L'x') != wstring::npos || inputString.find(L'X') != wstring::npos)
    {
        returnValue = stoll(inputString, &firstUnconvertedOffset, 16);
    }
    else
    {
        returnValue = stoll(inputString, &firstUnconvertedOffset, 10);
    }

    if (firstUnconvertedOffset != inputString.length())
    {
        throw invalid_argument(ctString::ctConvertToString(inputString));
    }
    return returnValue;
}

template <>
uint64_t ConvertToIntegral<uint64_t>(const wstring& inputString)
{
    auto returnValue = 0ull;
    size_t firstUnconvertedOffset = 0;
    if (inputString.find(L'x') != wstring::npos || inputString.find(L'X') != wstring::npos)
    {
        returnValue = stoull(inputString, &firstUnconvertedOffset, 16);
    }
    else
    {
        returnValue = stoull(inputString, &firstUnconvertedOffset, 10);
    }

    if (firstUnconvertedOffset != inputString.length())
    {
        throw invalid_argument(ctString::ctConvertToString(inputString));
    }
    return returnValue;
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the connect function to use
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForCreate(const vector<const wchar_t*>&)
{
    if (nullptr == g_configSettings->CreateFunction)
    {
        g_configSettings->CreateFunction = ctsWSASocket;
        g_createFunctionName = L"WSASocket";
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the connect function to use
///
/// -conn:connect
/// -conn:wsaconnect
/// -conn:wsaconnectbyname
/// -conn:connectex  (*default)
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForConnect(vector<const wchar_t*>& args)
{
    auto connectSpecifed = false;
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArg = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-conn");
        return value != nullptr;
    });
    if (foundArg != end(args))
    {
        if (g_configSettings->Protocol != ProtocolType::TCP)
        {
            throw invalid_argument("-conn (only applicable to TCP)");
        }

        // ReSharper disable once CppTooWideScopeInitStatement
        const auto* const value = ParseArgument(*foundArg, L"-conn");
        if (ctString::ctOrdinalEqualsCaseInsensative(L"ConnectEx", value))
        {
            g_configSettings->ConnectFunction = ctsConnectEx;
            g_connectFunctionName = L"ConnectEx";
        }
        else if (ctString::ctOrdinalEqualsCaseInsensative(L"connect", value))
        {
            g_configSettings->ConnectFunction = ctsSimpleConnect;
            g_connectFunctionName = L"connect";
        }
        else
        {
            throw invalid_argument("-conn");
        }
        connectSpecifed = true;
        // always remove the arg from our vector
        args.erase(foundArg);
    }
    else
    {
        if (g_configSettings->IoPattern != IoPatternType::MediaStream)
        {
            g_configSettings->ConnectFunction = ctsConnectEx;
            g_connectFunctionName = L"ConnectEx";
        }
        else
        {
            g_configSettings->ConnectFunction = ctsMediaStreamClientConnect;
            g_connectFunctionName = L"MediaStream Client Connect";
        }
    }

    if (IoPatternType::MediaStream == g_configSettings->IoPattern && connectSpecifed)
    {
        throw invalid_argument("-conn (MediaStream has its own internal connection handler)");
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the accept function to use
///
/// -acc:accept
/// -acc:wsaaccept
/// -acc:acceptex  (*default)
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForAccept(vector<const wchar_t*>& args)
{
    g_configSettings->AcceptLimit = c_defaultAcceptExLimit;

    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-acc");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        if (g_configSettings->Protocol != ProtocolType::TCP)
        {
            throw invalid_argument("-acc (only applicable to TCP)");
        }

        // ReSharper disable once CppTooWideScopeInitStatement
        const auto* const value = ParseArgument(*foundArgument, L"-acc");
        if (ctString::ctOrdinalEqualsCaseInsensative(L"accept", value))
        {
            g_configSettings->AcceptFunction = ctsSimpleAccept;
            g_acceptFunctionName = L"accept";
        }
        else if (ctString::ctOrdinalEqualsCaseInsensative(L"AcceptEx", value))
        {
            g_configSettings->AcceptFunction = ctsAcceptEx;
            g_acceptFunctionName = L"AcceptEx";
        }
        else
        {
            throw invalid_argument("-acc");
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
    else if (!g_configSettings->ListenAddresses.empty())
    {
        if (IoPatternType::MediaStream != g_configSettings->IoPattern)
        {
            // only default an Accept function if listening
            g_configSettings->AcceptFunction = ctsAcceptEx;
            g_acceptFunctionName = L"AcceptEx";
        }
        else
        {
            g_configSettings->AcceptFunction = ctsMediaStreamServerListener;
            g_acceptFunctionName = L"MediaStream Server Listener";
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the IO (read/write) function to use
/// -- only applicable to TCP
///
/// -io:blocking
/// -io:nonblocking
/// -io:event
/// -io:iocp (*default)
/// -io:wsapoll
/// -io:rioiocp
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForIoFunction(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-io");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        if (g_configSettings->Protocol != ProtocolType::TCP)
        {
            throw invalid_argument("-io (only applicable to TCP)");
        }

        // ReSharper disable once CppTooWideScopeInitStatement
        const auto* const value = ParseArgument(*foundArgument, L"-io");
        if (ctString::ctOrdinalEqualsCaseInsensative(L"iocp", value))
        {
            g_configSettings->IoFunction = ctsSendRecvIocp;
            g_configSettings->Options |= HandleInlineIocp;
            g_ioFunctionName = L"Iocp (WSASend/WSARecv using IOCP)";
        }
        else if (ctString::ctOrdinalEqualsCaseInsensative(L"readwritefile", value))
        {
            g_configSettings->IoFunction = ctsReadWriteIocp;
            g_ioFunctionName = L"ReadWriteFile (ReadFile/WriteFile using IOCP)";
        }
        else if (ctString::ctOrdinalEqualsCaseInsensative(L"rioiocp", value))
        {
            g_configSettings->IoFunction = ctsRioIocp;
            WI_SetFlag(g_configSettings->SocketFlags, WSA_FLAG_REGISTERED_IO);
            g_ioFunctionName = L"RioIocp (RIO using IOCP notifications)";
        }
        else
        {
            throw invalid_argument("-io");
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
    else
    {
        if (ProtocolType::TCP == g_configSettings->Protocol)
        {
            // Default for TCP is WSASend/WSARecv using IOCP
            g_configSettings->IoFunction = ctsSendRecvIocp;
            g_configSettings->Options |= HandleInlineIocp;
            g_ioFunctionName = L"Iocp (WSASend/WSARecv using IOCP)";
        }
        else
        {
            if (IsListening())
            {
                g_configSettings->IoFunction = ctsMediaStreamServerIo;
                // server also has a closing function to remove the closed socket
                g_configSettings->ClosingFunction = ctsMediaStreamServerClose;
                g_ioFunctionName = L"MediaStream Server";
            }
            else
            {
                constexpr auto udpRecvBuff = 1048576ul; // 1 MB
                g_configSettings->IoFunction = ctsMediaStreamClient;
                g_configSettings->Options |= SetRecvBuf;
                g_configSettings->RecvBufValue = udpRecvBuff;
                g_configSettings->Options |= HandleInlineIocp;
                g_configSettings->Options |= EnableCircularQueueing;
                g_ioFunctionName = L"MediaStream Client";
            }
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the InlineCompletions setting to use
///
/// -InlineCompletions:on
/// -InlineCompletions:off
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForInlineCompletions(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-inlinecompletions");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        // ReSharper disable once CppTooWideScopeInitStatement
        const auto* const value = ParseArgument(*foundArgument, L"-inlinecompletions");
        if (ctString::ctOrdinalEqualsCaseInsensative(L"on", value))
        {
            g_configSettings->Options |= HandleInlineIocp;
        }
        else if (ctString::ctOrdinalEqualsCaseInsensative(L"off", value))
        {
            g_configSettings->Options &= ~HandleInlineIocp;
        }
        else
        {
            throw invalid_argument("-inlinecompletions");
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the MsgWaitAll setting to use
///
/// -MsgWaitAll:on
/// -MsgWaitAll:off
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForMsgWaitAll(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-msgwaitall");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        // ReSharper disable once CppTooWideScopeInitStatement
        const auto* const value = ParseArgument(*foundArgument, L"-msgwaitall");
        if (ctString::ctOrdinalEqualsCaseInsensative(L"on", value))
        {
            g_configSettings->Options |= MsgWaitAll;
        }
        else if (ctString::ctOrdinalEqualsCaseInsensative(L"off", value))
        {
            g_configSettings->Options &= ~MsgWaitAll;
        }
        else
        {
            throw invalid_argument("-msgwaitall");
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
    {
        // default to enable msgwaitall
        g_configSettings->Options |= MsgWaitAll;
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the L4 Protocol to limit to usage
///
/// -Protocol:tcp
/// -Protocol:udp
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForProtocol(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-Protocol");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        // ReSharper disable once CppTooWideScopeInitStatement
        const auto* const value = ParseArgument(*foundArgument, L"-Protocol");
        if (ctString::ctOrdinalEqualsCaseInsensative(L"tcp", value))
        {
            g_configSettings->Protocol = ProtocolType::TCP;
        }
        else if (ctString::ctOrdinalEqualsCaseInsensative(L"udp", value))
        {
            g_configSettings->Protocol = ProtocolType::UDP;
        }
        else
        {
            throw invalid_argument("-Protocol");
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
    else
    {
        // default to TCP
        g_configSettings->Protocol = ProtocolType::TCP;
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for socket Options
/// - allows for more than one option to be set
/// -Options:<keepalive,tcpfastpath [-Options:<...>] [-Options:<...>]
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForOptions(vector<const wchar_t*>& args)
{
    for (;;)
    {
        // loop until cannot fine -Options
        // ReSharper disable once CppTooWideScopeInitStatement
        const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
            const auto* const value = ParseArgument(parameter, L"-Options");
            return value != nullptr;
        });

        if (foundArgument != end(args))
        {
            // ReSharper disable once CppTooWideScopeInitStatement
            const auto* const value = ParseArgument(*foundArgument, L"-Options");
            if (ctString::ctOrdinalEqualsCaseInsensative(L"keepalive", value))
            {
                if (ProtocolType::TCP == g_configSettings->Protocol)
                {
                    g_configSettings->Options |= Keepalive;
                }
                else
                {
                    throw invalid_argument("-Options (keepalive only allowed with TCP sockets)");
                }
            }
            else if (ctString::ctOrdinalEqualsCaseInsensative(L"tcpfastpath", value))
            {
                if (ProtocolType::TCP == g_configSettings->Protocol)
                {
                    g_configSettings->Options |= LoopbackFastPath;
                }
                else
                {
                    throw invalid_argument("-Options (tcpfastpath only allowed with TCP sockets)");
                }
            }
            else
            {
                throw invalid_argument("-Options");
            }

            // always remove the arg from our vector
            args.erase(foundArgument);
        }
        else
        {
            // didn't find -Options
            break;
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses the optional -KeepAliveValue:####
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForKeepAlive(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-keepalivevalue");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        if (ProtocolType::TCP == g_configSettings->Protocol)
        {
            g_configSettings->KeepAliveValue = ConvertToIntegral<uint32_t>(ParseArgument(*foundArgument, L"-keepalivevalue"));
            if (0 == g_configSettings->KeepAliveValue)
            {
                throw invalid_argument("Invalid KeepAliveValue");
            }
        }
        else
        {
            throw invalid_argument("-KeepAliveValue is only allowed with TCP sockets");
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the wire-Protocol to use
/// --- these only apply to TCP
///
/// -pattern:push
/// -pattern:pull
/// -pattern:pushpull
/// -pattern:duplex
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForIoPattern(vector<const wchar_t*>& args)
{
    auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-pattern");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        if (g_configSettings->Protocol != ProtocolType::TCP)
        {
            throw invalid_argument("-pattern (only applicable to TCP)");
        }

        // ReSharper disable once CppTooWideScopeInitStatement
        const auto* const value = ParseArgument(*foundArgument, L"-pattern");
        if (ctString::ctOrdinalEqualsCaseInsensative(L"push", value))
        {
            g_configSettings->IoPattern = IoPatternType::Push;
        }
        else if (ctString::ctOrdinalEqualsCaseInsensative(L"pull", value))
        {
            g_configSettings->IoPattern = IoPatternType::Pull;
        }
        else if (ctString::ctOrdinalEqualsCaseInsensative(L"pushpull", value))
        {
            g_configSettings->IoPattern = IoPatternType::PushPull;
        }
        else if (ctString::ctOrdinalEqualsCaseInsensative(L"flood", value) || ctString::ctOrdinalEqualsCaseInsensative(L"duplex", value))
        {
            // the old name for this was 'flood'
            g_configSettings->IoPattern = IoPatternType::Duplex;
        }
        else
        {
            throw invalid_argument("-pattern");
        }

        // always remove the arg from our vector
        args.erase(foundArgument);
    }
    else
    {
        if (g_configSettings->Protocol == ProtocolType::UDP)
        {
            g_configSettings->IoPattern = IoPatternType::MediaStream;
        }
        else
        {
            // default the TCP pattern to Push
            g_configSettings->IoPattern = IoPatternType::Push;
        }
    }

    // Now look for options tightly coupled to Protocol
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundPushbytes = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-pushbytes");
        return value != nullptr;
    });
    if (foundPushbytes != end(args))
    {
        if (g_configSettings->IoPattern != IoPatternType::PushPull)
        {
            throw invalid_argument("-PushBytes can only be set with -Pattern:PushPull");
        }
        g_configSettings->PushBytes = ConvertToIntegral<uint32_t>(ParseArgument(*foundPushbytes, L"-pushbytes"));
        // always remove the arg from our vector
        args.erase(foundPushbytes);
    }
    else
    {
        g_configSettings->PushBytes = c_defaultPushBytes;
    }

    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundPullbytes = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-pullbytes");
        return value != nullptr;
    });
    if (foundPullbytes != end(args))
    {
        if (g_configSettings->IoPattern != IoPatternType::PushPull)
        {
            throw invalid_argument("-PullBytes can only be set with -Pattern:PushPull");
        }
        g_configSettings->PullBytes = ConvertToIntegral<uint32_t>(ParseArgument(*foundPullbytes, L"-pullbytes"));
        // always remove the arg from our vector
        args.erase(foundPullbytes);
    }
    else
    {
        g_configSettings->PullBytes = c_defaultPullBytes;
    }

    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundBurstCount = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-burstcount");
        return value != nullptr;
    });
    if (foundBurstCount != end(args))
    {
        if (g_configSettings->Protocol != ProtocolType::TCP)
        {
            throw invalid_argument("-BurstCount requires -Protocol:TCP");
        }

        g_configSettings->BurstCount = ConvertToIntegral<uint32_t>(ParseArgument(*foundBurstCount, L"-burstcount"));
        if (g_configSettings->BurstCount == 0ul)
        {
            throw invalid_argument("-BurstCount requires a non-zero value");
        }
        // always remove the arg from our vector
        args.erase(foundBurstCount);
    }

    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundBurstDelay = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-burstdelay");
        return value != nullptr;
    });
    if (foundBurstDelay != end(args))
    {
        if (g_configSettings->Protocol != ProtocolType::TCP)
        {
            throw invalid_argument("-BurstDelay requires -Protocol:TCP");
        }

        g_configSettings->BurstDelay = ConvertToIntegral<uint32_t>(ParseArgument(*foundBurstDelay, L"-burstdelay"));
        if (g_configSettings->BurstDelay == 0ul)
        {
            throw invalid_argument("-BurstDelay requires a non-zero value");
        }
        // always remove the arg from our vector
        args.erase(foundBurstDelay);
    }

    // ReSharper disable CppRedundantParentheses
    if ((g_configSettings->BurstCount.has_value() && !g_configSettings->BurstDelay.has_value()) ||
        (!g_configSettings->BurstCount.has_value() && g_configSettings->BurstDelay.has_value()))
    {
        throw invalid_argument("-BurstCount and -BurstDelay must both be set if either are set");
    }
    // ReSharper restore CppRedundantParentheses

    //
    // Options for the UDP protocol
    //

    foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-BitsPerSecond");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        if (g_configSettings->Protocol != ProtocolType::UDP)
        {
            throw invalid_argument("-BitsPerSecond requires -Protocol:UDP");
        }
        g_mediaStreamSettings.BitsPerSecond = ConvertToIntegral<int64_t>(ParseArgument(*foundArgument, L"-BitsPerSecond"));
        // bitspersecond must align on a byte-boundary
        if (g_mediaStreamSettings.BitsPerSecond % 8 != 0)
        {
            g_mediaStreamSettings.BitsPerSecond -= g_mediaStreamSettings.BitsPerSecond % 8;
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }

    foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-FrameRate");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        if (g_configSettings->Protocol != ProtocolType::UDP)
        {
            throw invalid_argument("-FrameRate requires -Protocol:UDP");
        }
        g_mediaStreamSettings.FramesPerSecond = ConvertToIntegral<uint32_t>(ParseArgument(*foundArgument, L"-FrameRate"));
        // always remove the arg from our vector
        args.erase(foundArgument);
    }

    foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-BufferDepth");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        if (g_configSettings->Protocol != ProtocolType::UDP)
        {
            throw invalid_argument("-BufferDepth requires -Protocol:UDP");
        }
        g_mediaStreamSettings.BufferDepthSeconds = ConvertToIntegral<uint32_t>(ParseArgument(*foundArgument, L"-BufferDepth"));
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
    else
    {
        // default buffer depth to 1
        g_mediaStreamSettings.BufferDepthSeconds = 1;
    }

    foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-StreamLength");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        if (g_configSettings->Protocol != ProtocolType::UDP)
        {
            throw invalid_argument("-StreamLength requires -Protocol:UDP");
        }
        g_mediaStreamSettings.StreamLengthSeconds = ConvertToIntegral<uint32_t>(ParseArgument(*foundArgument, L"-StreamLength"));
        // always remove the arg from our vector
        args.erase(foundArgument);
    }

    // validate and resolve the UDP protocol options
    if (ProtocolType::UDP == g_configSettings->Protocol)
    {
        if (0 == g_mediaStreamSettings.BitsPerSecond)
        {
            throw invalid_argument("-BitsPerSecond is required");
        }
        if (0 == g_mediaStreamSettings.FramesPerSecond)
        {
            throw invalid_argument("-FrameRate is required");
        }
        if (0 == g_mediaStreamSettings.StreamLengthSeconds)
        {
            throw invalid_argument("-StreamLength is required");
        }

        // finally calculate the total stream length after all settings are captured from the user
        g_transferSizeLow = g_mediaStreamSettings.CalculateTransferSize();
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for IP address or machine name target to use
/// Can be comma-delimited if more than one
///
/// 3 different parameters read address/name settings:
/// Supports specifying the parameter multiple times:
///   e.g. -target:machinea -target:machineb
///
/// -listen: (address to listen on)
///   - specifying * == listen to all addresses
/// -target: (address to connect to)
/// -bind:   (address to bind before connecting)
///   - specifying * == bind to all addresses (default)
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForAddress(vector<const wchar_t*>& args)
{
    // -listen:<addr> 
    auto foundListen = begin(args);
    while (foundListen != end(args))
    {
        foundListen = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
            const auto* const value = ParseArgument(parameter, L"-listen");
            return value != nullptr;
        });
        if (foundListen != end(args))
        {
            // ReSharper disable once CppTooWideScopeInitStatement
            const auto* const value = ParseArgument(*foundListen, L"-listen");
            if (ctString::ctOrdinalEqualsCaseInsensative(L"*", value))
            {
                // add both v4 and v6
                ctSockaddr listenAddr(AF_INET, ctSockaddr::AddressType::Any);
                g_configSettings->ListenAddresses.push_back(listenAddr);
                listenAddr.set(AF_INET6, ctSockaddr::AddressType::Any);
                g_configSettings->ListenAddresses.push_back(listenAddr);
            }
            else
            {
                auto tempAddresses(ctSockaddr::ResolveName(value));
                if (tempAddresses.empty())
                {
                    throw invalid_argument("-listen value did not resolve to an IP address");
                }
                g_configSettings->ListenAddresses.insert(end(g_configSettings->ListenAddresses), begin(tempAddresses), end(tempAddresses));
            }
            // always remove the arg from our vector
            args.erase(foundListen);
            // found_listen is now invalidated since we just erased what it's pointing to
            // - reset it to begin() since we know it's not end()
            foundListen = args.begin();
        }
    }

    // -target:<addr> 
    auto foundTarget = begin(args);
    while (foundTarget != end(args))
    {
        foundTarget = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
            const auto* const value = ParseArgument(parameter, L"-target");
            return value != nullptr;
        });
        if (foundTarget != end(args))
        {
            if (!g_configSettings->ListenAddresses.empty())
            {
                throw invalid_argument("cannot specify both -Listen and -Target");
            }
            const auto* const value = ParseArgument(*foundTarget, L"-target");
            auto tempAddresses(ctSockaddr::ResolveName(value));
            if (tempAddresses.empty())
            {
                throw invalid_argument("-target value did not resolve to an IP address");
            }
            g_configSettings->TargetAddresses.insert(end(g_configSettings->TargetAddresses), begin(tempAddresses), end(tempAddresses));
            // always remove the arg from our vector
            args.erase(foundTarget);
            // found_target is now invalidated since we just erased what it's pointing to
            // - reset it to begin() since we know it's not end()
            foundTarget = args.begin();
        }
    }

    // -bind:<addr> 
    auto foundBind = begin(args);
    while (foundBind != end(args))
    {
        foundBind = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
            const auto* const value = ParseArgument(parameter, L"-bind");
            return value != nullptr;
        });
        if (foundBind != end(args))
        {
            // ReSharper disable once CppTooWideScopeInitStatement
            const auto* const value = ParseArgument(*foundBind, L"-bind");
            // check for a comma-delimited list of IP Addresses
            if (ctString::ctOrdinalEqualsCaseInsensative(L"*", value))
            {
                // add both v4 and v6
                ctSockaddr bindAddr(AF_INET, ctSockaddr::AddressType::Any);
                g_configSettings->BindAddresses.push_back(bindAddr);
                bindAddr.set(AF_INET6, ctSockaddr::AddressType::Any);
                g_configSettings->BindAddresses.push_back(bindAddr);
            }
            else
            {
                auto tempAddresses(ctSockaddr::ResolveName(value));
                if (tempAddresses.empty())
                {
                    throw invalid_argument("-bind value did not resolve to an IP address");
                }
                g_configSettings->BindAddresses.insert(end(g_configSettings->BindAddresses), begin(tempAddresses), end(tempAddresses));
            }
            // always remove the arg from our vector
            args.erase(foundBind);
            // found_bind is now invalidated since we just erased what it's pointing to
            // - reset it to begin() since we know it's not end()
            foundBind = args.begin();
        }
    }

    if (!g_configSettings->ListenAddresses.empty() && !g_configSettings->TargetAddresses.empty())
    {
        throw invalid_argument("cannot specify both -target and -listen");
    }
    if (!g_configSettings->ListenAddresses.empty() && !g_configSettings->BindAddresses.empty())
    {
        throw invalid_argument("cannot specify both -bind and -listen");
    }
    if (g_configSettings->ListenAddresses.empty() && g_configSettings->TargetAddresses.empty())
    {
        throw invalid_argument("must specify either -target or -listen");
    }

    // default bind addresses if not listening and did not exclusively want to bind
    if (g_configSettings->ListenAddresses.empty() && g_configSettings->BindAddresses.empty())
    {
        ctSockaddr defaultAddr(AF_INET, ctSockaddr::AddressType::Any);
        g_configSettings->BindAddresses.push_back(defaultAddr);
        defaultAddr.set(AF_INET6, ctSockaddr::AddressType::Any);
        g_configSettings->BindAddresses.push_back(defaultAddr);
    }

    if (!g_configSettings->TargetAddresses.empty())
    {
        //
        // guarantee that bindaddress and targetaddress families can match
        // - can't allow a bind address to be chosen if there are no TargetAddresses with the same family
        //
        uint32_t bindV4 = 0;
        uint32_t bindV6 = 0;
        uint32_t targetV4 = 0;
        uint32_t targetV6 = 0;
        for (const auto& addr : g_configSettings->BindAddresses)
        {
            if (addr.family() == AF_INET)
            {
                ++bindV4;
            }
            else
            {
                ++bindV6;
            }
        }
        for (const auto& addr : g_configSettings->TargetAddresses)
        {
            if (addr.family() == AF_INET)
            {
                ++targetV4;
            }
            else
            {
                ++targetV6;
            }
        }
        //
        // if either bind or target has zero of either family, remove those addrs from the other vector
        //
        if (0 == bindV4)
        {
            std::erase_if(
                g_configSettings->TargetAddresses,
                [](const ctSockaddr& addr) noexcept { return addr.family() == AF_INET; }
            );
        }
        else if (0 == targetV4)
        {
            std::erase_if(
                g_configSettings->BindAddresses,
                [](const ctSockaddr& addr) noexcept { return addr.family() == AF_INET; }
            );
        }

        if (0 == bindV6)
        {
            std::erase_if(
                g_configSettings->TargetAddresses,
                [](const ctSockaddr& addr) noexcept { return addr.family() == AF_INET6; }
            );
        }
        else if (0 == targetV6)
        {
            std::erase_if(
                g_configSettings->BindAddresses,
                [](const ctSockaddr& addr) noexcept { return addr.family() == AF_INET6; }
            );
        }
        //
        // now if either are of size zero, the user specified addresses which didn't align
        //
        if (g_configSettings->BindAddresses.empty() || g_configSettings->TargetAddresses.empty())
        {
            throw invalid_argument("-bind addresses and target addresses must match families");
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the Port # to listen to/connect to
///
/// -Port:##
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForPort(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-Port");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        g_configSettings->Port = ConvertToIntegral<WORD>(ParseArgument(*foundArgument, L"-Port"));
        if (0 == g_configSettings->Port)
        {
            throw invalid_argument("-Port");
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the connection limit [max number of connections to maintain]
///
/// -connections:####
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForConnections(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-connections");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        if (IsListening())
        {
            throw invalid_argument("-Connections is only supported when running as a client");
        }
        g_configSettings->ConnectionLimit = ConvertToIntegral<uint32_t>(ParseArgument(*foundArgument, L"-connections"));
        if (0 == g_configSettings->ConnectionLimit)
        {
            throw invalid_argument("-connections");
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the server limit [max number of connections before the server exits]
///
/// -ServerExitLimit:####
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForServerExitLimit(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-ServerExitLimit");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        if (!IsListening())
        {
            throw invalid_argument("-ServerExitLimit is only supported when running as a client");
        }
        g_configSettings->ServerExitLimit = ConvertToIntegral<ULONGLONG>(ParseArgument(*foundArgument, L"-ServerExitLimit"));
        if (0 == g_configSettings->ServerExitLimit)
        {
            // zero indicates no exit
            g_configSettings->ServerExitLimit = MAXULONGLONG;
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the connection limit [max number of connections to maintain]
///
/// -throttleconnections:####
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForThrottleConnections(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-throttleconnections");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        if (IsListening())
        {
            throw invalid_argument("-ThrottleConnections is only supported when running as a client");
        }
        g_configSettings->ConnectionThrottleLimit = ConvertToIntegral<uint32_t>(ParseArgument(*foundArgument, L"-throttleconnections"));
        if (0 == g_configSettings->ConnectionThrottleLimit)
        {
            // zero means no limit
            g_configSettings->ConnectionThrottleLimit = MAXUINT32;
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

template <typename T>
void ReadRangeValues(_In_z_ const wchar_t* value, T& outLow, T& outHigh)
{
    // a range was specified
    // - find the ',' the '[', and the ']'
    const auto valueLength = wcslen(value);
    const auto* const valueEnd = value + valueLength;
    if (valueLength < 5 || value[0] != L'[' || value[valueLength - 1] != L']')
    {
        throw invalid_argument("range value [###,###]");
    }
    const auto* const commaDelimiter = find(value, valueEnd, L',');
    if (!(valueEnd > commaDelimiter + 1))
    {
        throw invalid_argument("range value [###,###]");
    }

    // null-terminate the first number at the delimiter to do a string -> int conversion
    *const_cast<wchar_t*>(commaDelimiter) = L'\0';
    const auto* const valueLow = value + 1; // move past the '['
    outLow = ConvertToIntegral<T>(valueLow);

    // null-terminate for the 2nd number over the last ']' to doa string -> int conversion
    const_cast<wchar_t*>(value)[valueLength - 1] = L'\0';
    const auto* const valueHigh = commaDelimiter + 1;
    outHigh = ConvertToIntegral<T>(valueHigh);

    // validate buffer values
    if (outHigh < outLow)
    {
        throw invalid_argument("range value [###,###]");
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the buffer size to push down per IO
///
/// -buffer:####
///        :[low,high]
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForBuffer(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-buffer");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        if (g_configSettings->Protocol != ProtocolType::TCP)
        {
            throw invalid_argument("-buffer (only applicable to TCP)");
        }

        // ReSharper disable once CppTooWideScopeInitStatement
        const auto* const value = ParseArgument(*foundArgument, L"-buffer");
        if (value[0] == L'[')
        {
            ReadRangeValues(value, g_bufferSizeLow, g_bufferSizeHigh);
        }
        else
        {
            // singe values are written to g_BufferSizeLow, with g_BufferSizeHigh left at zero
            g_bufferSizeLow = ConvertToIntegral<uint32_t>(value);
        }
        if (0 == g_bufferSizeLow)
        {
            throw invalid_argument("-buffer");
        }

        // always remove the arg from our vector
        args.erase(foundArgument);
    }
    else
    {
        g_bufferSizeLow = c_defaultBufferSize;
        g_bufferSizeHigh = 0;
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the total transfer size in bytes per connection
///
/// -transfer:####
///          :[low,high]
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForTransfer(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-transfer");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        if (g_configSettings->Protocol != ProtocolType::TCP)
        {
            throw invalid_argument("-transfer (only applicable to TCP)");
        }

        // ReSharper disable once CppTooWideScopeInitStatement
        const auto* const value = ParseArgument(*foundArgument, L"-transfer");
        if (value[0] == L'[')
        {
            ReadRangeValues(value, g_transferSizeLow, g_transferSizeHigh);
        }
        else
        {
            // singe values are written to g_TransferSizeLow, with g_TransferSizeHigh left at zero
            g_transferSizeLow = ConvertToIntegral<uint64_t>(value);
        }
        if (0 == g_transferSizeLow)
        {
            throw invalid_argument("-transfer");
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the LocalPort # to bind for local connect
/// 
/// -LocalPort:##
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForLocalport(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-LocalPort");
        return value != nullptr;
    });

    if (foundArgument != end(args))
    {
        // ReSharper disable once CppTooWideScopeInitStatement
        const auto* const value = ParseArgument(*foundArgument, L"-LocalPort");
        if (value[0] == L'[')
        {
            ReadRangeValues(value, g_configSettings->LocalPortLow, g_configSettings->LocalPortHigh);
        }
        else
        {
            // single value are written to localport_low with localport_high left at zero
            g_configSettings->LocalPortHigh = 0;
            g_configSettings->LocalPortLow = ConvertToIntegral<unsigned short>(value);
        }
        if (0 == g_configSettings->LocalPortLow)
        {
            throw invalid_argument("-LocalPort");
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for an explicitly specified interface index for outgoing connections
/// 
/// -IfIndex:##
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForIfIndex(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-IfIndex");
        return value != nullptr;
    });

    if (foundArgument != end(args))
    {
        const auto* const value = ParseArgument(*foundArgument, L"-IfIndex");
        g_configSettings->OutgoingIfIndex = ConvertToIntegral<uint32_t>(value);

        if (0 == g_configSettings->OutgoingIfIndex)
        {
            throw invalid_argument("-IfIndex");
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for Tcp throttling parameters
///
/// -RateLimit:####
///           :[low,high]
/// -RateLimitPeriod:####
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForRatelimit(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundRatelimit = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-RateLimit");
        return value != nullptr;
    });
    if (foundRatelimit != end(args))
    {
        if (g_configSettings->Protocol != ProtocolType::TCP)
        {
            throw invalid_argument("-RateLimit (only applicable to TCP)");
        }
        // ReSharper disable once CppTooWideScopeInitStatement
        const auto* const value = ParseArgument(*foundRatelimit, L"-RateLimit");
        if (value[0] == L'[')
        {
            ReadRangeValues(value, g_rateLimitLow, g_rateLimitLow);
        }
        else
        {
            // singe values are written to g_BufferSizeLow, with g_BufferSizeHigh left at zero
            g_rateLimitLow = ConvertToIntegral<int64_t>(ParseArgument(*foundRatelimit, L"-RateLimit"));
        }
        if (0LL == g_rateLimitLow)
        {
            throw invalid_argument("-RateLimit");
        }
        // always remove the arg from our vector
        args.erase(foundRatelimit);
    }

    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundRatelimitPeriod = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-RateLimitPeriod");
        return value != nullptr;
    });
    if (foundRatelimitPeriod != end(args))
    {
        if (g_configSettings->Protocol != ProtocolType::TCP)
        {
            throw invalid_argument("-RateLimitPeriod (only applicable to TCP)");
        }
        if (0LL == g_rateLimitLow)
        {
            throw invalid_argument("-RateLimitPeriod requires specifying -RateLimit");
        }
        g_configSettings->TcpBytesPerSecondPeriod = ConvertToIntegral<int64_t>(ParseArgument(*foundRatelimitPeriod, L"-RateLimitPeriod"));
        // always remove the arg from our vector
        args.erase(foundRatelimitPeriod);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the total # of iterations
///
/// -Iterations:####
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForIterations(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-Iterations");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        if (IsListening())
        {
            throw invalid_argument("-Iterations is only supported when running as a client");
        }
        g_configSettings->Iterations = ConvertToIntegral<ULONGLONG>(ParseArgument(*foundArgument, L"-Iterations"));
        if (0 == g_configSettings->Iterations)
        {
            g_configSettings->Iterations = MAXULONGLONG;
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the verbosity level
///
/// -ConsoleVerbosity:## <0-6>
/// -StatusUpdate:####
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForLogging(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundVerbosity = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-ConsoleVerbosity");
        return value != nullptr;
    });
    if (foundVerbosity != end(args))
    {
        g_consoleVerbosity = ConvertToIntegral<uint32_t>(ParseArgument(*foundVerbosity, L"-ConsoleVerbosity"));
        if (g_consoleVerbosity > 6)
        {
            throw invalid_argument("-ConsoleVerbosity");
        }
        // always remove the arg from our vector
        args.erase(foundVerbosity);
    }

    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundStatusUpdate = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-StatusUpdate");
        return value != nullptr;
    });
    if (foundStatusUpdate != end(args))
    {
        g_configSettings->StatusUpdateFrequencyMilliseconds = ConvertToIntegral<uint32_t>(ParseArgument(*foundStatusUpdate, L"-StatusUpdate"));
        if (0 == g_configSettings->StatusUpdateFrequencyMilliseconds)
        {
            throw invalid_argument("-StatusUpdate");
        }
        // always remove the arg from our vector
        args.erase(foundStatusUpdate);
    }

    wstring connectionFilename;
    wstring errorFilename;
    wstring statusFilename;
    wstring jitterFilename;
    wstring tcpInfoFilename;

    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundConnectionFilename = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-ConnectionFilename");
        return value != nullptr;
    });
    if (foundConnectionFilename != end(args))
    {
        connectionFilename = ParseArgument(*foundConnectionFilename, L"-ConnectionFilename");
        // always remove the arg from our vector
        args.erase(foundConnectionFilename);
    }

    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundErrorFilename = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-ErrorFilename");
        return value != nullptr;
    });
    if (foundErrorFilename != end(args))
    {
        errorFilename = ParseArgument(*foundErrorFilename, L"-ErrorFilename");
        // always remove the arg from our vector
        args.erase(foundErrorFilename);
    }

    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundStatusFilename = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-StatusFilename");
        return value != nullptr;
    });
    if (foundStatusFilename != end(args))
    {
        statusFilename = ParseArgument(*foundStatusFilename, L"-StatusFilename");
        // always remove the arg from our vector
        args.erase(foundStatusFilename);
    }

    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundJitterFilename = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-JitterFilename");
        return value != nullptr;
    });
    if (foundJitterFilename != end(args))
    {
        jitterFilename = ParseArgument(*foundJitterFilename, L"-JitterFilename");
        // always remove the arg from our vector
        args.erase(foundJitterFilename);
    }

    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundTcpInfoFilename = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-TcpInfoFilename");
        return value != nullptr;
    });
    if (foundTcpInfoFilename != end(args))
    {
        tcpInfoFilename = ParseArgument(*foundTcpInfoFilename, L"-TcpInfoFilename");
        // always remove the arg from our vector
        args.erase(foundTcpInfoFilename);
    }

    // since CSV files each have their own header, we cannot allow the same CSV filename to be used
    // for different loggers, as opposed to txt files, which can be shared across different loggers

    if (!connectionFilename.empty())
    {
        if (ctString::ctOrdinalEndsWithCaseInsensative(connectionFilename, L".csv"))
        {
            g_connectionLogger = make_shared<ctsTextLogger>(connectionFilename.c_str(), StatusFormatting::Csv);
        }
        else
        {
            g_connectionLogger = make_shared<ctsTextLogger>(connectionFilename.c_str(), StatusFormatting::ClearText);
        }
    }

    if (!errorFilename.empty())
    {
        if (ctString::ctOrdinalEqualsCaseInsensative(connectionFilename, errorFilename))
        {
            if (g_connectionLogger->IsCsvFormat())
            {
                throw invalid_argument("The error logfile cannot be of csv format");
            }
            g_errorLogger = g_connectionLogger;
        }
        else
        {
            if (ctString::ctOrdinalEndsWithCaseInsensative(errorFilename, L".csv"))
            {
                throw invalid_argument("The error logfile cannot be of csv format");
            }
            g_errorLogger = make_shared<ctsTextLogger>(errorFilename.c_str(), StatusFormatting::ClearText);
        }
    }

    if (!statusFilename.empty())
    {
        if (ctString::ctOrdinalEqualsCaseInsensative(connectionFilename, statusFilename))
        {
            if (g_connectionLogger->IsCsvFormat())
            {
                throw invalid_argument("The same csv filename cannot be used for different loggers");
            }
            g_statusLogger = g_connectionLogger;
        }
        else if (ctString::ctOrdinalEqualsCaseInsensative(errorFilename, statusFilename))
        {
            if (g_errorLogger->IsCsvFormat())
            {
                throw invalid_argument("The same csv filename cannot be used for different loggers");
            }
            g_statusLogger = g_errorLogger;
        }
        else
        {
            if (ctString::ctOrdinalEndsWithCaseInsensative(statusFilename, L".csv"))
            {
                g_statusLogger = make_shared<ctsTextLogger>(statusFilename.c_str(), StatusFormatting::Csv);
            }
            else
            {
                g_statusLogger = make_shared<ctsTextLogger>(statusFilename.c_str(), StatusFormatting::ClearText);
            }
        }
    }

    if (!jitterFilename.empty())
    {
        if (ctString::ctOrdinalEndsWithCaseInsensative(jitterFilename, L".csv"))
        {
            if (ctString::ctOrdinalEqualsCaseInsensative(connectionFilename, jitterFilename) ||
                ctString::ctOrdinalEqualsCaseInsensative(errorFilename, jitterFilename) ||
                ctString::ctOrdinalEqualsCaseInsensative(statusFilename, jitterFilename))
            {
                throw invalid_argument("The same csv filename cannot be used for different loggers");
            }
            g_jitterLogger = make_shared<ctsTextLogger>(jitterFilename.c_str(), StatusFormatting::Csv);
        }
        else
        {
            throw invalid_argument("Jitter can only be logged using a csv format");
        }
    }

    if (!tcpInfoFilename.empty())
    {
        if (ctString::ctOrdinalEndsWithCaseInsensative(tcpInfoFilename, L".csv"))
        {
            if (ctString::ctOrdinalEqualsCaseInsensative(connectionFilename, tcpInfoFilename) ||
                ctString::ctOrdinalEqualsCaseInsensative(errorFilename, tcpInfoFilename) ||
                ctString::ctOrdinalEqualsCaseInsensative(statusFilename, tcpInfoFilename) ||
                ctString::ctOrdinalEqualsCaseInsensative(jitterFilename, tcpInfoFilename))
            {
                throw invalid_argument("The same csv filename cannot be used for different loggers");
            }
            g_tcpInfoLogger = make_shared<ctsTextLogger>(tcpInfoFilename.c_str(), StatusFormatting::Csv);
        }
        else
        {
            throw invalid_argument("TCP Info can only be logged using a csv format");
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Sets error policy
///
/// -OnError:<log,break>
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForError(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-OnError");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        // ReSharper disable once CppTooWideScopeInitStatement
        const auto* const value = ParseArgument(*foundArgument, L"-OnError");
        if (ctString::ctOrdinalEqualsCaseInsensative(L"log", value))
        {
            g_breakOnError = false;
        }
        else if (ctString::ctOrdinalEqualsCaseInsensative(L"break", value))
        {
            g_breakOnError = true;
        }
        else
        {
            throw invalid_argument("-OnError");
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Sets optional prepostrecvs value
///
/// -PrePostRecvs:#####
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForPrepostrecvs(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-PrePostRecvs");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        g_configSettings->PrePostRecvs = ConvertToIntegral<uint32_t>(ParseArgument(*foundArgument, L"-PrePostRecvs"));
        if (0 == g_configSettings->PrePostRecvs)
        {
            throw invalid_argument("-PrePostRecvs");
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
    else
    {
        g_configSettings->PrePostRecvs = ProtocolType::TCP == g_configSettings->Protocol ? 1 : 2;
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Sets optional prepostsends value
///
/// -PrePostSends:#####
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForPrepostsends(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-PrePostSends");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        g_configSettings->PrePostSends = ConvertToIntegral<uint32_t>(ParseArgument(*foundArgument, L"-PrePostSends"));
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
    else
    {
        g_configSettings->PrePostSends = 1;
        if (WI_IsFlagSet(g_configSettings->SocketFlags, WSA_FLAG_REGISTERED_IO))
        {
            // 0 PrePostSends == rely on ISB
            g_configSettings->PrePostSends = 0;
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Sets optional SO_RCVBUF value
///
/// -RecvBufValue:#####
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForRecvbufvalue(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-RecvBufValue");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        g_configSettings->RecvBufValue = ConvertToIntegral<uint32_t>(ParseArgument(*foundArgument, L"-RecvBufValue"));
        g_configSettings->Options |= SetRecvBuf;
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Sets optional SO_SNDBUF value
///
/// -SendBufValue:#####
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForSendbufvalue(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-SendBufValue");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        g_configSettings->SendBufValue = ConvertToIntegral<uint32_t>(ParseArgument(*foundArgument, L"-SendBufValue"));
        g_configSettings->Options |= SetSendBuf;
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Sets an IP Compartment (routing domain)
///
/// -Compartment:<ifalias>
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForCompartment(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-Compartment");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        // delay-load IPHLPAPI.DLL
        const auto* const value = ParseArgument(*foundArgument, L"-Compartment");
        g_netAdapterAddresses = new ctNetAdapterAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_ALL_COMPARTMENTS);
        const auto foundInterface = ranges::find_if(*g_netAdapterAddresses,
            [&value](const IP_ADAPTER_ADDRESSES& adapterAddress) {
                return ctString::ctOrdinalEqualsCaseInsensative(value, adapterAddress.FriendlyName);
            });
        THROW_HR_IF_MSG(
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND),
            foundInterface == g_netAdapterAddresses->end(),
            "GetAdaptersAddresses could not find the interface alias '%ws'",
            value);

        g_compartmentId = foundInterface->CompartmentId;
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Sets a threadpool environment for TP APIs to consume
///
/// Configuring for max threads == number of processors * 2
///
/// currently not exposing this as a command-line parameter
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForThreadpool(vector<const wchar_t*>& args)
{
    auto setRunsLong = false;
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-threadpool");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        // ReSharper disable once CppTooWideScopeInitStatement
        const auto* const value = ParseArgument(*foundArgument, L"-threadpool");
        if (ctString::ctOrdinalEqualsCaseInsensative(L"default", value))
        {
            setRunsLong = false;
        }
        else if (ctString::ctOrdinalEqualsCaseInsensative(L"runslong", value))
        {
            setRunsLong = true;
        }

        // always remove the arg from our vector
        args.erase(foundArgument);
    }

    SYSTEM_INFO systemInfo;
    GetSystemInfo(&systemInfo);
    g_threadPoolThreadCount = systemInfo.dwNumberOfProcessors * c_defaultThreadpoolFactor;

    g_threadPool = CreateThreadpool(nullptr);
    if (!g_threadPool)
    {
        THROW_WIN32_MSG(GetLastError(), "CreateThreadPool");
    }
    SetThreadpoolThreadMaximum(g_threadPool, g_threadPoolThreadCount);

    InitializeThreadpoolEnvironment(&g_threadPoolEnvironment);
    if (setRunsLong)
    {
        SetThreadpoolCallbackRunsLong(&g_threadPoolEnvironment);
    }
    SetThreadpoolCallbackPool(&g_threadPoolEnvironment, g_threadPool);

    g_configSettings->pTpEnvironment = &g_threadPoolEnvironment;
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for whether to verify buffer contents on receiver
///
/// -verify:<connection,data>
/// (the old options were <always,never>)
///
/// Note this controls if using a SharedBuffer across all IO or unique buffers
/// - if not validating data, won't waste memory creating buffers for every connection
/// - if validating data, must create buffers for every connection
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForShouldVerifyBuffers(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-verify");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        // ReSharper disable once CppTooWideScopeInitStatement
        const auto* const value = ParseArgument(*foundArgument, L"-verify");
        if (ctString::ctOrdinalEqualsCaseInsensative(L"always", value) || ctString::ctOrdinalEqualsCaseInsensative(L"data", value))
        {
            g_configSettings->ShouldVerifyBuffers = true;
            g_configSettings->UseSharedBuffer = false;
        }
        else if (ctString::ctOrdinalEqualsCaseInsensative(L"never", value) || ctString::ctOrdinalEqualsCaseInsensative(L"connection", value))
        {
            g_configSettings->ShouldVerifyBuffers = false;
            g_configSettings->UseSharedBuffer = true;
        }
        else
        {
            throw invalid_argument("-verify");
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for how the client should close the connection with the server
///
/// -shutdown:<graceful,rude>
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForShutdown(vector<const wchar_t*>& args)
{
    if (IsListening())
    {
        g_configSettings->TcpShutdown = TcpShutdownType::ServerSideShutdown;
    }

    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-shutdown");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        if (IsListening())
        {
            throw invalid_argument("-shutdown is a client-only option");
        }

        // ReSharper disable once CppTooWideScopeInitStatement
        const auto* const value = ParseArgument(*foundArgument, L"-shutdown");
        if (ctString::ctOrdinalEqualsCaseInsensative(L"graceful", value))
        {
            g_configSettings->TcpShutdown = TcpShutdownType::GracefulShutdown;
        }
        else if (ctString::ctOrdinalEqualsCaseInsensative(L"rude", value))
        {
            g_configSettings->TcpShutdown = TcpShutdownType::HardShutdown;
        }
        else
        {
            throw invalid_argument("-shutdown");
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
///
/// Parses for the optional maximum time to run
///
/// -TimeLimit:##
///
//////////////////////////////////////////////////////////////////////////////////////////
static void ParseForTimelimit(vector<const wchar_t*>& args)
{
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundArgument = ranges::find_if(args, [](const wchar_t* parameter) -> bool {
        const auto* const value = ParseArgument(parameter, L"-timelimit");
        return value != nullptr;
    });
    if (foundArgument != end(args))
    {
        g_configSettings->TimeLimit = ConvertToIntegral<uint32_t>(ParseArgument(*foundArgument, L"-timelimit"));
        if (0 == g_configSettings->Port)
        {
            throw invalid_argument("-timelimit");
        }
        // always remove the arg from our vector
        args.erase(foundArgument);
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
///
/// Members within the ctsConfig namespace that can be accessed anywhere within ctsTraffic
///
////////////////////////////////////////////////////////////////////////////////////////////////////
void PrintUsage(PrintUsageOption option)
{
    ctsConfigInitOnce();

    wstring usage;

    switch (option)
    {
        case PrintUsageOption::Default:
            usage.append(L"\n\n"
                L"ctsTraffic is a utility to generate and validate the integrity of network traffic. It is a client / server application "
                L"with the ability to send and receive traffic in a variety of protocol patterns, utilizing a variety of API calling patterns. "
                L"The protocol is validated in bytes sent and received for every connection established. Should there be any API failure, any "
                L"connection lost prematurely, any protocol failure in bytes sent or received, ctsTraffic will capture and log that error information. "
                L"Any errors will additionally cause ctsTraffic to return a non-zero error code.\n"
                L"Once started, ctrl-c or ctrl-break will cleanly shutdown the client or server\n"
                L"\n\n"
                L"For issues or questions, please contact 'ctsSupport'\n"
                L"\n\n"
                L"ctsTraffic -Help:[tcp] [udp] [logging] [advanced]\n"
                L"\t- <default> == prints this usage statement\n"
                L"\t- tcp : prints usage for TCP-specific options\n"
                L"\t- udp : prints usage for UDP-specific options\n"
                L"\t- logging : prints usage for logging options\n"
                L"\t- advanced : prints the usage for advanced and experimental options\n"
                L"\n\n"
                L"Server-side usage:\n"
                L"\tctsTraffic -Listen:<addr or *> [-Port:####] [-ServerExitLimit:<####>] [-Protocol:<tcp/udp>] [-Verify:####] [Protocol-specific options]\n"
                L"\n"
                L"Client-side usage:\n"
                L"\tctsTraffic -Target:<addr or name> [-Port:####] [-Connections:<####>] [-Iterations:<####>] [-Protocol:<tcp/udp>] [-Verify:####] [Protocol-specific options]\n"
                L"\n"
                L"The Server-side and Client-side may have fully independent settings *except* for the following:\n"
                L" (these must match exactly between the client and the server)\n"
                L"\t-Port\n"
                L"\t-Protocol\n"
                L"\t-Verify\n"
                L"\t-Pattern (on TCP)\n"
                L"\t-Transfer (on TCP)\n"
                L"\t-BitsPerSecond (on UDP)\n"
                L"\t-FrameRate (on UDP)\n"
                L"\t-StreamLength (on UDP)\n"
                L"\n\n"
                L"----------------------------------------------------------------------\n"
                L"                    Common Server-side options                        \n"
                L"----------------------------------------------------------------------\n"
                L"-Listen:<addr or *> [-Listen:<addr> -Listen:<addr>]\n"
                L"   - the specific IP Address for the server-side to listen, or '*' for all IP Addresses\n"
                L"\t- <required>\n"
                L"\t  note : can specify multiple addresses by providing -Listen for each address\n"
                L"-ServerExitLimit:####\n"
                L"   - the total # of accepted connections before server gracefully exits\n"
                L"\t- <default> == 0  (infinite)\n"
                L"\n\n"
                L"----------------------------------------------------------------------\n"
                L"                    Common Client-side options                        \n"
                L"----------------------------------------------------------------------\n"
                L"-Connections:####\n"
                L"   - the total # of connections at any one time\n"
                L"\t- <default> == 8  (there will always be 8 connections doing IO)\n"
                L"-Iterations:####\n"
                L"   - the number of times to iterate across the number of '-Connections'\n"
                L"\t- <default> == 0  (infinite)\n"
                L"\t  note : the total # of connections to be made before exit == Iterations * Connections\n"
                L"-Target:<addr or name>\n"
                L"   - the server-side IP Address, FQDN, or hostname to connect\n"
                L"\t- <required>\n"
                L"\t  note : given a FQDN or hostname, each new connection will iterate across\n"
                L"\t       : all IPv4 and IPv6 addresses which the name resolved\n"
                L"\t  note : one can specify '-Target:localhost' when client and server are both local\n"
                L"\t  note : one can specify multiple targets by providing -Target for each address or name\n"
                L"\n\n"
                L"----------------------------------------------------------------------\n"
                L"                    Common options for all roles                      \n"
                L"----------------------------------------------------------------------\n"
                L"-Port:####\n"
                L"   - the port # the server will listen and the client will connect\n"
                L"\t- <default> == 4444\n"
                L"-Protocol:<tcp,udp>\n"
                L"   - the protocol used for connectivity and IO\n"
                L"\t- tcp : see -help:TCP for usage options\n"
                L"\t- udp : see -help:UDP for usage options\n"
                L"-Verify:<connection,data>\n"
                L"   - an enumeration to indicate the level of integrity verification\n"
                L"\t- <default> == data\n"
                L"\t- connection : the integrity of every connection is verified\n"
                L"\t             : including the precise # of bytes to send and receive\n"
                L"\t- data : the integrity of every received data buffer is verified against the an expected bit-pattern\n"
                L"\t       : this validation is a superset of 'connection' integrity validation\n"
                L"\n");
            break;

        case PrintUsageOption::Tcp:
            usage.append(L"\n"
                L"----------------------------------------------------------------------\n"
                L"                    TCP-specific usage options                        \n"
                L"----------------------------------------------------------------------\n"
                L"-Buffer:#####\n"
                L"   - the # of bytes in the buffer used for each send/recv IO\n"
                L"\t- <default> == 65536  (each send or recv will post a 64KB buffer)\n"
                L"\t- supports range : [low,high]  (each connection will randomly choose a buffer size from within this range)\n"
                L"\t  note : Buffer is note required when -Pattern:MediaStream is specified,\n"
                L"\t       : FrameSize is the effective buffer size in that traffic pattern\n"
                L"-IO:<iocp,rioiocp>\n"
                L"   - the API set and usage for processing the protocol pattern\n"
                L"\t- <default> == iocp\n"
                L"\t- iocp : leverages WSARecv/WSASend using IOCP for async completions\n"
                L"\t- rioiocp : registered i/o using an overlapped IOCP for completion notification\n"
                L"-Pattern:<push,pull,pushpull,duplex,burst>\n"
                L"   - the protocol pattern to send & recv over the TCP connection\n"
                L"\t- <default> == push\n"
                L"\t- push : client pushes data to the server\n"
                L"\t- pull : client pulls data from the server\n"
                L"\t- pushpull : client/server alternates sending/receiving data\n"
                L"\t- duplex : client/server sends and receives concurrently throughout the entire connection\n"
                L"-PullBytes:#####\n"
                L"   - applied only with -Pattern:PushPull - the number of bytes to 'pull'\n"
                L"\t- <default> == 1048576 (1MB)\n"
                L"\t  note : pullbytes are the bytes received on the client and sent from the server\n"
                L"-PushBytes:#####\n"
                L"   - applied only with -Pattern:PushPull - the number of bytes to 'push'\n"
                L"\t- <default> == 1048576 (1MB)\n"
                L"\t  note : pushbytes are the bytes sent from the client and received on the server\n"
                L"-BurstCount:####\n"
                L"   - optional parameter\n"
                L"   - applies to any TCP IO Pattern\n"
                L"   - the number of sends() to send -buffer:#### in a tight loop before triggering a delay\n"
                L"\t  note : this is a required field when using BurstDelay\n"
                L"-BurstDelay:####\n"
                L"   - optional parameter\n"
                L"   - applies to any TCP IO Pattern\n"
                L"   - the number of milliseconds to delay after completing -BurstCount sends\n"
                L"\t  note : this is a required field when using BurstCount\n"
                L"-RateLimit:#####\n"
                L"   - rate limits the number of bytes/sec being *sent* on each individual connection\n"
                L"\t- <default> == 0 (no rate limits)\n"
                L"\t- supports range : [low,high]  (each connection will randomly choose a rate limit setting from within this range)\n"
                L"-Transfer:#####\n"
                L"   - the total bytes to transfer per TCP connection\n"
                L"\t- <default> == 1073741824  (each connection will transfer a sum total of 1GB)\n"
                L"\t- supports range : [low,high]  (each connection will randomly choose a total transfer size send across)\n"
                L"\t  note : specifying a range *will* create failures (used to test TCP failures paths)\n"
                L"-Shutdown:<graceful,rude>\n"
                L"   - controls how clients terminate the TCP connection - note this is a client-only option\n"
                L"\t- <default> == graceful\n"
                L"\t- graceful : client will initiate a 4-way FIN with the server and wait for the server's FIN\n"
                L"\t- rude : client will immediately close the connection once it receives the 'done' response from the server\n"
                L"         : this will deliberately tell TCP to linger for zero seconds and close the socket\n"
                L"         : this may reesult in a RST instead of a FIN\n"
                L"\n");
            break;

        case PrintUsageOption::Udp:
            usage.append(L"\n"
                L"----------------------------------------------------------------------\n"
                L"                    UDP-specific usage options                        \n"
                L"                                                                      \n"
                L"  * UDP datagrams are streamed in a controlled pattern                \n"
                L"    similarly to audio/video streaming solutions                      \n"
                L"  * In all cases, the client-side receives and server-side sends      \n"
                L"    at a fixed bit-rate and frame-size                                \n"
                L"----------------------------------------------------------------------\n"
                L"-BitsPerSecond:####\n"
                L"   - the number of bits per second to stream split across '-FrameRate' # of frames\n"
                L"\t- <required>\n"
                L"-FrameRate:####\n"
                L"   - the number of frames per second being streamed\n"
                L"\t- <required>\n"
                L"\t  note : for server-side this is the specific frequency that datagrams are sent\n"
                L"\t       : for client-side this is the frequency that frames are processed and verified\n"
                L"-StreamLength:####\n"
                L"   - the total number of seconds to run the entire stream\n"
                L"\t- <required>\n"
                L"-BufferDepth:####\n"
                L"   - the number of seconds to buffer before processing the stream\n"
                L"\t- <default> = 1 (second)\n"
                L"\t  note : this affects the client-side buffering of frames\n"
                L"\t       : this also affects how far the client-side will peek at frames to resend if missing\n"
                L"\t       : the client will look ahead at 1/2 the buffer depth to request a resend if missing\n"
                L"\n");
            break;

        case PrintUsageOption::Logging:
            usage.append(L"\n"
                L"----------------------------------------------------------------------\n"
                L"                    Logging options                                   \n"
                L"----------------------------------------------------------------------\n"
                L"Logging in ctsTraffic:\n"
                L"Information available to be logged is grouped into 4 basic buckets:\n"
                L"  - Connection information : this will write a data point for every successful connection established\n"
                L"                             -ConnectionFilename specifies the file written with this data\n"
                L"                             the IP address and port tuples for the source and destination will be written\n"
                L"                             this will also write a data point at the point of every connection completion\n"
                L"                             information unique to the protocol that was used will be included on success\n"
                L"  - Error information      : this will write error strings at the point of failure of any connection\n"
                L"                             -ErrorFilename specifies the file written with this data\n"
                L"                             error information will include the specific point of failure (function that failed)\n"
                L"                             as well as which connection the failure occurred (based off of IP address and port)\n"
                L"  - Status information     : this will write out status information as applicable to the protocol being used\n"
                L"                             -StatusFilename specifies the file written with this data\n"
                L"                             the status information will be printed at a frequency set by -StatusUpdate\n"
                L"                             the details printed are aggregate values from all connections for that time slice\n"
                L"  - Jitter information     : for UDP-patterns only, the jitter logging information will write out data per-datagram\n"
                L"                             -JitterFilename specifies the file written with this data\n"
                L"                             this information is formatted specifically to calculate jitter between packets\n"
                L"                             it follows the same format used with the published tool ntttcp.exe:\n"
                L"                             [frame#],[sender.qpc],[sender.qpf],[receiver.qpc],[receiver.qpf]\n"
                L"                             - qpc is the result of QueryPerformanceCounter\n"
                L"                             - qpf is the result of QueryPerformanceFrequency\n"
                L"                             the algorithm to apply to this data can be found on this site under 'Performance Metrics'\n"
                L"                             http://msdn.microsoft.com/en-us/library/windows/hardware/dn247504.aspx \n"
                L"  - TCP_INFO information   : for TCP-patterns only, the TcpInfo logging captures information from TCP_INFO_* structs\n"
                L"                             -TcpInfoFilename specifies the file written with this data\n"
                L"                             this information is captured at the end of each TCP connection and written to csv\n"
                L"                             note this is only available on Windows 10 RS2 and later\n"
                L"\n"
                L"The format in which the above data is logged is based off of the file extension of the filename specified above\n"
                L"  - There are 2 possible file types:\n"
                L"\t - txt : plain text format is used with the file extension .txt, or for an unrecognized file extension\n"
                L"\t         text output is formatted as one would see it printed to the console in UTF8 format\n"
                L"\t - csv : comma-separated value format is used with the file extension .csv\n"
                L"\t         information is separated into columns separated by a comma for easier post-processing\n"
                L"\t         the column layout of the data is specific to the type of output and protocol being used\n"
                L"\t         NOTE: csv formatting will only apply to status updates and jitter, not connection or error information\n"
                L"\n"
                L"\n"
                L"-ConsoleVerbosity:<0-5>\n"
                L"\t - logging verbosity for all information to be written to the console\n"
                L"\t   <default> == 4\n"
                L"\t   - 0 : off (nothing written to the console)\n"
                L"\t   - 1 : status updates\n"
                L"\t   - 2 : error information only\n"
                L"\t   - 3 : connection information only\n"
                L"\t   - 4 : connection information + error information\n"
                L"\t   - 5 : connection information + error information + status updates\n"
                // L"\t   - 6 : above + debug output\n" // Not exposing debug information to users
                L"-ConnectionFilename:<filename with/without path>\n"
                L"\t - <default> == not written to a log file\n"
                L"-ErrorFilename:<filename with/without path>\n"
                L"\t - <default> == not written to a log file\n"
                L"-StatusFilename:<filename with/without path>\n"
                L"\t - <default> == not written to a log file\n"
                L"-JitterFilename:<filename with/without path>\n"
                L"\t - <default> == not written to a log file\n"
                L"-TcpInfoFilename:<filename with/without path>\n"
                L"\t - <default> == not written to a log file\n"
                L"-StatusUpdate:####\n"
                L"\t - the millisecond frequency which real-time status updates are written\n"
                L"\t   <default> == 5000 (milliseconds)\n"
                L"\n");
            break;

        case PrintUsageOption::Advanced:
            usage.append(L"\n"
                L"----------------------------------------------------------------------\n"
                L"                        Advanced Options                              \n"
                L"                                                                      \n"
                L"  * these options target specific scenario requirements               \n"
                L"----------------------------------------------------------------------\n"
                L"-Acc:<accept,AcceptEx>\n"
                L"   - specifies the Winsock API to process accepting inbound connections\n"
                L"    the default is appropriate unless deliberately needing to test other APIs\n"
                L"\t- <default> == AcceptEx\n"
                L"\t- AcceptEx : uses OVERLAPPED AcceptEx with IO Completion ports\n"
                L"\t- accept : uses blocking calls to accept\n"
                L"\t         : be careful using this as it will not scale out well as each call blocks a thread\n"
                L"-Bind:<IP-address or *>\n"
                L"   - a client-side option used to control what IP address is used for outgoing connections\n"
                L"\t- <default> == *  (will implicitly bind to the correct IP to connect to the target IP)\n"
                L"\t  note : this is typically only necessary when wanting to distribute traffic\n"
                L"\t         over a specific interface for multi-homed configurations\n"
                L"\t  note : can specify multiple addresses by providing -Bind for each address\n"
                L"-Compartment:<ifAlias>\n"
                L"   - specifies the interface alias of the compartment to use for all sockets\n"
                L"    this is most commonly appropriate for servers configured with IP Compartments\n"
                L"\t- <default> == using the default IP compartment\n"
                L"\t  note : all systems use the default compartment unless explicitly configured otherwise\n"
                L"\t  note : the IP addresses specified through -Bind (for clients) and -Listen (for servers)\n"
                L"\t         will be directly affected by this Compartment value, including specifying '*'\n"
                L"-Conn:<connect,ConnectEx>\n"
                L"   - specifies the Winsock API to establish outbound connections\n"
                L"    the default is appropriate unless deliberately needing to test other APIs\n"
                L"\t- <default> == ConnectEx  (appropriate unless explicitly wanting to test other APIs)\n"
                L"\t- ConnectEx : uses OVERLAPPED ConnectEx with IO Completion ports\n"
                L"\t- connect : uses blocking calls to connect\n"
                L"\t          : be careful using this as it will not scale out well as each call blocks a thread\n"
                L"-IfIndex:####\n"
                L"   - the interface index which to use for outbound connectivity\n"
                L"     assigns the interface with IP_UNICAST_IF / IPV6_UNICAST_IF\n"
                L"\t- <default> == not set (will not restrict binding to any specific interface)\n"
                L"-InlineCompletions:<on,off>\n"
                L"   - will set the below option on all SOCKETS for OVERLAPPED I/O calls so inline successful\n"
                L"     completions will not be queued to the completion handler\n"
                L"     ::SetFileCompletionNotificationModes(FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)\n"
                L"\t- <default> == on for TCP 'iocp' -IO option, and is on for UDP client receivers\n"
                L"                 off for all other -IO options\n"
                L"-IO:<readwritefile>\n"
                L"   - an additional IO option beyond iocp and rioiocp\n"
                L"\t- readwritefile : leverages ReadFile/WriteFile using IOCP for async completions\n"
                L"-KeepAliveValue:####\n"
                L"   - the # of milliseconds to set KeepAlive for TCP connections\n"
                L"\t- <default> == not set\n"
                L"\t  note : This setting is a more specific setting than -Options:keepalive\n"
                L"\t         as -Options:keepalive will use the system default values for keep-alive timers\n"
                L"-LocalPort:####\n"
                L"   - the local port to bind to when initiating a connection\n"
                L"\t- <default> == 0  (an ephemeral port will be chosen when making a connection)\n"
                L"\t- supports range : [low,high] each new connection will sequentially choose a port within this range\n"
                L"\t  note : You must provide a sufficiently large range to support the number of connections\n"
                L"\t  note : Be very careful when using with TCP connections, as port values will not be immediately\n"
                L"\t         reusable; TCP will hold an closed IP:port in a TIME_WAIT statue for a period of time\n"
                L"\t         only after which will it be able to be reused (default is 4 minutes)\n"
                L"-MsgWaitAll:<on,off>\n"
                L"   - sets the MSG_WAITALL flag when calling WSARecv for receiving data over TCP connections\n"
                L"     this flag instructs TCP to not complete the receive request until the entire buffer is full\n"
                L"\t- <default> == on\n"
                L"\t  note : the default behavior when not specified is for TCP to indicate data up to the app per RFC\n"
                L"           thus apps generally only set this when they know precisely the number of bytes they are expecting\n"
                L"-OnError:<log,break>\n"
                L"   - policy to control how errors are handled at runtime\n"
                L"\t- <default> == log \n"
                L"\t- log : log error information only\n"
                L"\t- break : break into the debugger with error information\n"
                L"\t          useful when live-troubleshooting difficult failures\n"
                L"-Options:<keepalive,tcpfastpath>  [-Options:<...>] [-Options:<...>]\n"
                L"   - additional socket options and IOCTLS available to be set on connected sockets\n"
                L"\t- <default> == None\n"
                L"\t- keepalive : only for TCP sockets - enables default timeout Keep-Alive probes\n"
                L"\t            : ctsTraffic servers have this enabled by default\n"
                L"\t- tcpfastpath : a new option for Windows 8, only for TCP sockets over loopback\n"
                L"\t              : the firewall must be disabled for the option to take effect\n"
                L"-PrePostRecvs:#####\n"
                L"   - specifies the number of recv requests to issue concurrently within an IO Pattern\n"
                L"   - for example, with the default -pattern:pull, the client will post recv calls \n"
                L"\t     one after another, immediately posting a recv after the prior completed.\n"
                L"\t     with -pattern:pull -PrePostRecvs:2, clients will keep 2 recv calls in-flight at all times.\n"
                L"\t- <default> == 1 for TCP (one recv request at a time)\n"
                L"\t- <default> == 2 for UDP (two recv requests kept in-flight)\n"
                L"\t  note : with TCP patterns, -verify:connection must be specified in order to specify\n"
                L"\t         more than one -PrePostRecvs (UDP can always support any number)\n"
                L"-PrePostSends:#####\n"
                L"   - specifies the number of send requests to issue concurrently within an IO Pattern\n"
                L"   - for example, with the default -pattern:pull, the servers will post send calls \n"
                L"\t     one after another, immediately posting a send after the prior completed.\n"
                L"\t     With -pattern:pull -PrePostSends:2, servers will keep 2 send calls in-flight at all times.\n"
                L"   - The value of '0' has special meaning: it indicates for ctsTraffic to keep as many sends\n"
                L"\t     in flight as indicated by the Ideal Send Backlog (ISB) indicated by TCP. In this\n"
                L"\t     configuration, ctsTraffic will maintain send calls until the number of bytes being sent\n"
                L"\t     equals the number of byes indicates by ISB for that TCP connection.\n"
                L"\t- <default> == 1 for non-RIO TCP (Winsock will adjust automatically according to ISB)\n"
                L"\t- <default> == 0 (ISB) for RIO TCP (RIO doesn't user send buffers so callers must track ISB)\n"
                L"\t- <default> == 1 for UDP (one send request on each timer tick)\n"
                L"-RateLimitPeriod:#####\n"
                L"   - the # of milliseconds describing the granularity by which -RateLimit bytes/second is enforced\n"
                L"\t     the -RateLimit bytes/second will be evenly split across -RateLimitPeriod milliseconds\n"
                L"\t     For example, -RateLimit:1000 -RateLimitPeriod:50 will limit send rates to 100 bytes every 20 ms\n"
                L"\t- <default> == 100 (-RateLimit bytes/second will be split out across 100 ms. time slices)\n"
                L"\t  note : only applicable to TCP connections\n"
                L"\t  note : only applicable is -RateLimit is set (default is not to rate limit)\n"
                L"-RecvBufValue:#####\n"
                L"   - specifies the value to pass to the SO_RCVBUF socket option\n"
                L"\t     Note: this is only necessary to specify in carefully considered scenarios\n"
                L"\t     the default receive buffering is optimal for the majority of scenarios\n"
                L"\t- <default> == <not set>\n"
                L"-SendBufValue:#####\n"
                L"   - specifies the value to pass to the SO_SNDBUF socket option\n"
                L"\t     Note: this is only necessary to specify in carefully considered scenarios\n"
                L"\t     the default send buffering is optimal for the majority of scenarios\n"
                L"\t- <default> == <not set>\n"
                L"-ThrottleConnections:####\n"
                L"   - gates currently pended connection attempts\n"
                L"\t- <default> == 1000  (there will be at most 1000 sockets trying to connect at any one time)\n"
                L"\t  note : zero means no throttling  (will immediately try to connect all '-Connections')\n"
                L"\t       : this is a client-only option\n"
                L"-Threadpool:<default,runslong>\n"
                L"   - sets options on the NT threadpool used for IO and work items\n"
                L"\t- <default> == default\n"
                L"\t- default : uses the default TP_CALLBACK_ENVIRON from InitializeThreadpoolEnvironment\n"
                L"\t            this is recommended for must use cases unless we see work not well distributed\n"
                L"\t            between different CPUs - usually only at very high throughput rates\n"
                L"\t- runslong : calls SetThreadpoolCallbackRunsLong on the TP_CALLBACK_ENVIRON\n"
                L"-TimeLimit:#####\n"
                L"   - the maximum number of milliseconds to run before the application is aborted and terminated\n"
                L"\t- <default> == <no time limit>\n"
                L"\t  note : this is to be used only to cap the maximum time to run, as this will log an error\n"
                L"\t         if this timelimit is exceeded; predictable results should have the scenario finish\n"
                L"\t         before this time limit is hit\n"
                L"\n");
            break;
    }

    fwprintf_s(stdout, L"%ws", usage.c_str());
}

bool Startup(int argc, _In_reads_(argc) const wchar_t** argv)
{
    ctsConfigInitOnce();

    if (argc < 2)
    {
        PrintUsage();
        return false;
    }

    // ignore the first argv... the exe itself
    const wchar_t** argBegin = argv + 1;
    const wchar_t** argEnd = argv + argc;
    vector args(argBegin, argEnd);

    //
    // first check of they asked for help text
    //
    // ReSharper disable once CppTooWideScopeInitStatement
    const auto foundHelp = ranges::find_if(args,
        [](const wchar_t* arg) -> bool {
            return ctString::ctOrdinalStartsWithCaseInsensative(arg, L"-Help") ||
                   ctString::ctOrdinalEqualsCaseInsensative(arg, L"-?");
        });
    if (foundHelp != end(args))
    {
        const auto* helpString = *foundHelp;
        if (ctString::ctOrdinalEqualsCaseInsensative(helpString, L"-Help:Advanced"))
        {
            PrintUsage(PrintUsageOption::Advanced);
            return false;
        }
        if (ctString::ctOrdinalEqualsCaseInsensative(helpString, L"-Help:Tcp"))
        {
            PrintUsage(PrintUsageOption::Tcp);
            return false;
        }
        if (ctString::ctOrdinalEqualsCaseInsensative(helpString, L"-Help:Udp"))
        {
            PrintUsage(PrintUsageOption::Udp);
            return false;
        }
        if (ctString::ctOrdinalEqualsCaseInsensative(helpString, L"-Help:Logging"))
        {
            PrintUsage(PrintUsageOption::Logging);
            return false;
        }

        PrintUsage();
        return false;
    }

    //
    // create the handle for ctrl-c
    //
    g_configSettings->CtrlCHandle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_configSettings->CtrlCHandle)
    {
        THROW_WIN32_MSG(GetLastError(), "CreateEvent");
    }

    //
    // Many of the below settings must be made in a specified order - comments below help to explain this reasoning
    // note: the IO function definitions must come after *all* other settings
    //       since instantiations of those IO functions might reference global Settings values

    //
    // First:
    // Establish logging settings including verbosity levels and error policies before any functional settings
    // Create the threadpool before instantiating any other object
    //
    ParseForError(args);
    ParseForLogging(args);

    //
    // Next: check for static machine configuration
    // - note these are checking system settings, not user arguments
    //
    CheckSystemSettings();

    //
    // Next: establish the address and port # to be used
    //
    ParseForAddress(args);
    ParseForPort(args);
    ParseForLocalport(args);
    ParseForIfIndex(args);

    //
    // ensure a Port is assigned to all listening addresses and target addresses
    //
    for (auto& addr : g_configSettings->ListenAddresses)
    {
        if (addr.port() == 0x0000)
        {
            addr.SetPort(g_configSettings->Port);
        }
    }
    for (auto& addr : g_configSettings->TargetAddresses)
    {
        if (addr.port() == 0x0000)
        {
            addr.SetPort(g_configSettings->Port);
        }
    }

    if (g_configSettings->OutgoingIfIndex != 0 && !g_configSettings->ListenAddresses.empty())
    {
        throw invalid_argument("-IfIndex can only be used for outgoing connections, not listening sockets");
    }

    //
    // Next: gather the protocol and Pattern to be used
    // - set the threadpool value after identifying the pattern
    ParseForProtocol(args);
    // default to keep-alive on TCP servers
    if (ProtocolType::TCP == g_configSettings->Protocol && !g_configSettings->ListenAddresses.empty())
    {
        g_configSettings->Options |= Keepalive;
    }

    ParseForIoPattern(args);
    ParseForThreadpool(args);
    // validate protocol & pattern combinations
    if (ProtocolType::UDP == g_configSettings->Protocol && IoPatternType::MediaStream != g_configSettings->IoPattern)
    {
        throw invalid_argument("UDP only supports the MediaStream IO Pattern");
    }
    if (ProtocolType::TCP == g_configSettings->Protocol && IoPatternType::MediaStream == g_configSettings->IoPattern)
    {
        throw invalid_argument("TCP does not support the MediaStream IO Pattern");
    }
    // set appropriate defaults for # of connections for TCP vs. UDP
    if (ProtocolType::UDP == g_configSettings->Protocol)
    {
        g_configSettings->ConnectionLimit = c_defaultUdpConnectionLimit;
    }
    else
    {
        g_configSettings->ConnectionLimit = c_defaultTcpConnectionLimit;
    }

    //
    // Next, set the ctsStatusInformation to be used to print status updates for this protocol
    // - this must be called after both set_logging and set_protocol
    //
    if (ProtocolType::TCP == g_configSettings->Protocol)
    {
        g_printStatusInformation = make_shared<ctsTcpStatusInformation>();
    }
    else
    {
        g_printStatusInformation = make_shared<ctsUdpStatusInformation>();
    }

    //
    // Next: capture other various settings which do not have explicit dependencies
    //
    ParseForOptions(args);
    ParseForKeepAlive(args);
    ParseForCompartment(args);
    ParseForConnections(args);
    ParseForThrottleConnections(args);
    ParseForBuffer(args);
    ParseForTransfer(args);
    ParseForIterations(args);
    ParseForServerExitLimit(args);

    ParseForRatelimit(args);
    ParseForTimelimit(args);

    if (g_rateLimitLow > 0LL && g_configSettings->BurstDelay.has_value())
    {
        throw invalid_argument("-RateLimit and -Burstdelay cannot be used concurrently");
    }

    // ReSharper disable once CppTooWideScopeInitStatement
    const auto ratePerPeriod = g_rateLimitLow * g_configSettings->TcpBytesPerSecondPeriod / 1000LL;
    if (g_configSettings->Protocol == ProtocolType::TCP && g_rateLimitLow > 0 && ratePerPeriod < 1)
    {
        throw invalid_argument("RateLimit * RateLimitPeriod / 1000 must be greater than zero - meaning every period should send at least 1 byte");
    }

    //
    // verify jitter logging requirements
    //
    if (g_jitterLogger && g_configSettings->Protocol != ProtocolType::UDP)
    {
        throw invalid_argument("Jitter can only be logged using UDP");
    }
    if (g_jitterLogger && !g_configSettings->ListenAddresses.empty())
    {
        throw invalid_argument("Jitter can only be logged on the client");
    }
    if (g_jitterLogger && g_configSettings->ConnectionLimit != 1)
    {
        throw invalid_argument("Jitter can only be logged for a single UDP connection");
    }

    if (g_mediaStreamSettings.FrameSizeBytes > 0)
    {
        // the buffersize is now effectively the frame size
        g_bufferSizeHigh = 0;
        g_bufferSizeLow = g_mediaStreamSettings.FrameSizeBytes;
        if (g_bufferSizeLow < 20)
        {
            throw invalid_argument("The media stream frame size (buffer) must be at least 20 bytes");
        }
    }

    // validate localport usage
    if (!g_configSettings->ListenAddresses.empty() && g_configSettings->LocalPortLow != 0)
    {
        throw invalid_argument("Cannot specify both -listen and -LocalPort. To listen on a specific port, use -Port:####");
    }
    if (g_configSettings->LocalPortLow != 0)
    {
        // ReSharper disable once CppTooWideScopeInitStatement
        const USHORT numberOfPorts = g_configSettings->LocalPortHigh == 0 ? 1 : static_cast<USHORT>(g_configSettings->LocalPortHigh - g_configSettings->LocalPortLow + 1);
        if (numberOfPorts < g_configSettings->ConnectionLimit)
        {
            throw invalid_argument(
                "Cannot specify more connections than specified local ports. "
                "Reduce the number of connections or increase the range of local ports.");
        }
    }

    //
    // Set the default buffer values as these settings are optional
    //
    g_configSettings->ShouldVerifyBuffers = true;
    g_configSettings->UseSharedBuffer = false;
    ParseForShouldVerifyBuffers(args);
    if (ProtocolType::UDP == g_configSettings->Protocol)
    {
        // UDP clients can never recv into the same shared buffer since it uses it for seq. numbers, etc
        if (!IsListening())
        {
            g_configSettings->UseSharedBuffer = false;
        }
    }

    //
    // finally set the functions to use once all other settings are established
    // set_ioFunction changes global options for socket operation for instance WSA_FLAG_REGISTERED_IO flag
    // - hence it is requirement to invoke it prior to any socket operation
    //
    ParseForIoFunction(args);
    ParseForInlineCompletions(args);
    ParseForMsgWaitAll(args);
    ParseForCreate(args);
    ParseForConnect(args);
    ParseForAccept(args);
    if (!g_configSettings->ListenAddresses.empty())
    {
        // servers 'create' connections when they accept them
        g_configSettings->CreateFunction = g_configSettings->AcceptFunction;
        g_configSettings->ConnectFunction = nullptr;
    }

    g_configSettings->TcpShutdown = TcpShutdownType::GracefulShutdown;
    ParseForShutdown(args);

    ParseForPrepostrecvs(args);
    if (ProtocolType::TCP == g_configSettings->Protocol && g_configSettings->ShouldVerifyBuffers && g_configSettings->PrePostRecvs > 1)
    {
        throw invalid_argument("-PrePostRecvs > 1 requires -Verify:connection when using TCP");
    }
    ParseForPrepostsends(args);
    ParseForRecvbufvalue(args);
    ParseForSendbufvalue(args);

    if (!args.empty())
    {
        wstring errorString;
        for (const auto& argString : args)
        {
            errorString.append(L" ");
            errorString.append(argString);
        }
        errorString.append(L"\n");
        PrintErrorInfoOverride(errorString.c_str());
        throw invalid_argument(ctString::ctConvertToString(errorString).c_str());
    }

    if (ProtocolType::UDP == g_configSettings->Protocol)
    {
        if (const auto timerResult = timeBeginPeriod(1); timerResult != TIMERR_NOERROR)
        {
            THROW_WIN32_MSG(timerResult, "timeBeginPeriod");
        }
        ++g_timePeriodRefCount;
    }

    return true;
}

void Shutdown() noexcept
{
    ctsConfigInitOnce();

    const auto lock = g_shutdownLock.lock();
    g_shutdownCalled = true;
    if (g_configSettings->CtrlCHandle)
    {
        if (!SetEvent(g_configSettings->CtrlCHandle))
        {
            FAIL_FAST_MSG(
                "SetEvent(%p) failed [%u] when trying to shutdown",
                g_configSettings->CtrlCHandle, GetLastError());
        }
    }

    delete g_netAdapterAddresses;
    g_netAdapterAddresses = nullptr;

    while (g_timePeriodRefCount > 0)
    {
        timeEndPeriod(1);
        --g_timePeriodRefCount;
    }
}

// the Legend is to explain the fields for status updates
// - only print if status updates are going to be provided
void PrintLegend() noexcept
{
    ctsConfigInitOnce();

    auto writeToConsole = false;
    // ReSharper disable once CppDefaultCaseNotHandledInSwitchStatement
    switch (g_consoleVerbosity) // NOLINT(hicpp-multiway-paths-covered)
    {
        // case 0: // nothing
        case 1: // status updates
        // case 2: // error info
        // case 3: // connection info
        // case 4: // connection info + error info
        case 5: // connection info + error info + status updates
        case 6: // above + debug info
        {
            writeToConsole = true;
        }
    }

    if (g_printStatusInformation)
    {
        if (writeToConsole)
        {
            if (const auto* legend = g_printStatusInformation->PrintLegend(StatusFormatting::ConsoleOutput))
            {
                fwprintf(stdout, L"%ws\n", legend);
            }
            if (const auto* header = g_printStatusInformation->PrintHeader(StatusFormatting::ConsoleOutput))
            {
                fwprintf(stdout, L"%ws\n", header);
            }
        }

        if (g_statusLogger)
        {
            g_statusLogger->LogLegend(g_printStatusInformation);
            g_statusLogger->LogHeader(g_printStatusInformation);
        }
    }

    if (g_connectionLogger && g_connectionLogger->IsCsvFormat())
    {
        if (ProtocolType::UDP == g_configSettings->Protocol)
        {
            g_connectionLogger->LogMessage(L"TimeSlice,LocalAddress,RemoteAddress,Bits/Sec,Completed,Dropped,Repeated,Errors,Result,ConnectionId\r\n");
        }
        else
        {
            // TCP
            g_connectionLogger->LogMessage(L"TimeSlice,LocalAddress,RemoteAddress,SendBytes,SendBps,RecvBytes,RecvBps,TimeMs,Result,ConnectionId\r\n");
        }
    }

    if (g_jitterLogger && g_jitterLogger->IsCsvFormat())
    {
        g_jitterLogger->LogMessage(L"SequenceNumber,SenderQpc,SenderQpf,ReceiverQpc,ReceiverQpf,RelativeInFlightTimeMs,PrevToCurrentInFlightTimeJitter\r\n");
    }

    if (g_tcpInfoLogger && g_tcpInfoLogger->IsCsvFormat())
    {
        g_tcpInfoLogger->LogMessage(L"TimeSlice,LocalAddress,RemoteAddress,ConnectionId,SendBytes,SendBps,RecvBytes,RecvBps,TimeMs,BytesReordered,BytesRetransmitted,SynRetransmitted,DupAcksIn,MinRttUs,Mss,TimeoutEpisodes,FastRetransmit,SndLimBytesCwnd,SndLimBytesRwin,SndLimBytesSnd\r\n");
    }
}

// Always print to console if override
void PrintExceptionOverride(_In_ PCSTR exceptionText) noexcept
{
    ctsConfigInitOnce();

    FAIL_FAST_IF_MSG(g_breakOnError, "[ctsTraffic] >> exception - %hs\n", exceptionText);

    try
    {
        const auto formattedString(
            wil::str_printf<std::wstring>(
                L"[%.3f] %hs",
                GetStatusTimeStamp(),
                exceptionText));

        fwprintf(stderr, L"%ws\n", formattedString.c_str());
        if (g_errorLogger)
        {
            g_errorLogger->LogError(
                wil::str_printf<std::wstring>(L"%ws\r\n", formattedString.c_str()).c_str());
        }
    }
    catch (...)
    {
        fwprintf(stderr, L"Error : failed to allocate memory\n");
        if (g_errorLogger)
        {
            g_errorLogger->LogError(L"Error : failed to allocate memory\r\n");
        }
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////
///
/// Print* functions
/// - tracks what level of -verbose was specified
///   and prints to console accordingly
///
////////////////////////////////////////////////////////////////////////////////////////////////////
void PrintException(const wil::ResultException& e) noexcept
{
    ctsConfigInitOnce();

    WCHAR errorString[1024]{};
    GetFailureLogString(errorString, 1024, e.GetFailureInfo());
    if (!g_shutdownCalled && g_breakOnError)
    {
        FAIL_FAST_MSG(
            "Fatal exception: %ws", errorString);
    }

    PrintErrorInfo(errorString);
}

void PrintException(const std::exception& e) noexcept
{
    ctsConfigInitOnce();
    try
    {
        PrintErrorInfo(ctString::ctConvertToWstring(e.what()).c_str());
    }
    catch (...)
    {
        const auto hr = wil::ResultFromCaughtException();
        if (!g_shutdownCalled && g_breakOnError)
        {
            FAIL_FAST_MSG("Fatal exception: 0x%x", hr);
        }

        // ReSharper disable once CppDefaultCaseNotHandledInSwitchStatement
        switch (g_consoleVerbosity) // NOLINT(hicpp-multiway-paths-covered)
        {
            // case 0: // nothing
            // case 1: // status updates
            case 2: // error info
            // case 3: // connection info
            case 4: // connection info + error info
            case 5: // connection info + error info + status updates
            case 6: // above + debug info
                wprintf(
                    L"[%.3f] Exception thrown: %hs\n",
                    GetStatusTimeStamp(),
                    e.what());
        }
    }
}

DWORD PrintThrownException() noexcept
{
    try
    {
        throw;
    }
    catch (const wil::ResultException& e)
    {
        PrintException(e);
        return Win32FromHresult(e.GetErrorCode());
    }
    catch (const std::exception& e)
    {
        PrintException(e);
        return WSAENOBUFS;
    }
    catch (...)
    {
        FAIL_FAST();
    }
}

void PrintException(DWORD why, _In_ PCWSTR what, _In_ PCWSTR where) noexcept try
{
    const auto translation = ctString::ctFormatMessage(why);
    const auto formattedString = wil::str_printf<std::wstring>(
        L"[exception] %ws%ws%ws%ws [%u / 0x%x - %ws]",
        what ? L" " : L"",
        what ? what : L"",
        where ? L" at " : L"",
        where ? where : L"",
        why,
        why,
        !translation.empty() ? translation.c_str() : L"unknown error");
    PrintErrorInfo(formattedString.c_str());
}
catch (...)
{
    PrintErrorInfo(L"[exception] out of memory");
}

// Always print to console if override
void PrintErrorInfoOverride(_In_ PCWSTR text) noexcept try
{
    ctsConfigInitOnce();

    if (g_breakOnError)
    {
        FAIL_FAST_MSG(ctString::ctConvertToString(text).c_str());
    }

    wprintf_s(L"%ws\n", text);
    if (g_errorLogger)
    {
        g_errorLogger->LogError(
            wil::str_printf<std::wstring>(
                L"[%.3f] %ws\r\n",
                GetStatusTimeStamp(), text).c_str());
    }
}
catch (...)
{
}

void PrintErrorIfFailed(_In_ PCSTR what, uint32_t why) noexcept try
{
    ctsConfigInitOnce();

    if (!g_shutdownCalled && why != 0)
    {
        if (g_breakOnError)
        {
            FAIL_FAST_MSG("%hs failed (%u)\n", what, why);
        }

        auto writeToConsole = false;
        // ReSharper disable once CppDefaultCaseNotHandledInSwitchStatement
        switch (g_consoleVerbosity) // NOLINT(hicpp-multiway-paths-covered)
        {
            // case 0: // nothing
            // case 1: // status updates
            case 2: // error info
            // case 3: // connection info
            case 4: // connection info + error info
            case 5: // connection info + error info + status updates
            case 6: // above + debug info
            {
                writeToConsole = true;
            }
        }

        wstring errorString;
        if (ctsIoPattern::IsProtocolError(why))
        {
            errorString = wil::str_printf<std::wstring>(
                L"[%.3f] Connection aborted due to the protocol error %ws",
                GetStatusTimeStamp(),
                ctsIoPattern::BuildProtocolErrorString(why));
        }
        else
        {
            errorString = wil::str_printf<std::wstring>(
                L"[%.3f] %hs failed (%u) %ws",
                GetStatusTimeStamp(),
                what,
                why,
                ctString::ctFormatMessage(why).c_str());
        }

        if (writeToConsole)
        {
            fwprintf(stderr, L"%ws\n", errorString.c_str());
        }

        if (g_errorLogger)
        {
            g_errorLogger->LogError(
                wil::str_printf<std::wstring>(L"%ws\r\n", errorString.c_str()).c_str());
        }
    }
}
catch (...)
{
}

void PrintStatusUpdate() noexcept
{
    if (!g_shutdownCalled)
    {
        if (g_printStatusInformation)
        {
            auto writeToConsole = false;
            // ReSharper disable once CppDefaultCaseNotHandledInSwitchStatement
            switch (g_consoleVerbosity) // NOLINT(hicpp-multiway-paths-covered)
            {
                // case 0: // nothing
                case 1: // status updates
                // case 2: // error info
                // case 3: // connection info
                // case 4: // connection info + error info
                case 5: // connection info + error info + status updates
                case 6: // above + debug info
                {
                    writeToConsole = true;
                }
            }

            if (const auto lock = g_statusUpdateLock.try_lock())
            {
                // capture the timeslices
                const auto lPrevioutimeslice = g_previousPrintTimeslice;
                // ReSharper disable once CppTooWideScopeInitStatement
                const auto lCurrentTimeslice = ctTimer::SnapQpcInMillis() - g_configSettings->StartTimeMilliseconds;

                if (lCurrentTimeslice > lPrevioutimeslice)
                {
                    // write out the header to the console every 40 updates 
                    if (writeToConsole)
                    {
                        if (g_printTimesliceCount != 0 && 0 == g_printTimesliceCount % 40)
                        {
                            if (const auto* header = g_printStatusInformation->PrintHeader(StatusFormatting::ConsoleOutput))
                            {
                                fwprintf(stdout, L"%ws", header);
                            }
                        }
                    }

                    // need to indicate either print_status() or LogStatus() to reset the status info,
                    // - the data *must* be reset once and *only once* in this function

                    auto statusCount = 0;
                    if (writeToConsole)
                    {
                        ++statusCount;
                    }
                    if (g_statusLogger)
                    {
                        ++statusCount;
                    }

                    if (writeToConsole)
                    {
                        --statusCount;
                        const bool clearStatus = 0 == statusCount;
                        if (const auto* printString = g_printStatusInformation->PrintStatus(StatusFormatting::ConsoleOutput, lCurrentTimeslice, clearStatus))
                        {
                            fwprintf(stdout, L"%ws", printString);
                        }
                    }

                    if (g_statusLogger)
                    {
                        --statusCount;
                        const bool clearStatus = 0 == statusCount;
                        g_statusLogger->LogStatus(g_printStatusInformation, lCurrentTimeslice, clearStatus);
                    }

                    // update tracking values
                    g_previousPrintTimeslice = lCurrentTimeslice;
                    ++g_printTimesliceCount;
                }
            }
        }
    }
}

void PrintJitterUpdate(const JitterFrameEntry& currentFrame, const JitterFrameEntry& previousFrame) noexcept
{
    if (!g_shutdownCalled)
    {
        if (g_jitterLogger)
        {
            const auto jitter = std::abs(previousFrame.m_estimatedTimeInFlightMs - currentFrame.m_estimatedTimeInFlightMs);
            // int64_t ~= up to 20 characters long, 10 for each float, plus 10 for commas & CR
            constexpr size_t formattedTextLength = 20 * 5 + 10 * 2 + 10;
            wchar_t formattedText[formattedTextLength]{};
            const auto converted = _snwprintf_s(
                formattedText,
                formattedTextLength,
                L"%lld,%lld,%lld,%lld,%lld,%.3f,%.3f\r\n",
                currentFrame.m_sequenceNumber, currentFrame.m_senderQpc, currentFrame.m_senderQpf, currentFrame.m_receiverQpc, currentFrame.m_receiverQpf, currentFrame.m_estimatedTimeInFlightMs, jitter);
            FAIL_FAST_IF(-1 == converted);
            g_jitterLogger->LogMessage(formattedText);
        }
    }
}

void PrintNewConnection(const ctSockaddr& localAddr, const ctSockaddr& remoteAddr) noexcept try
{
    ctsConfigInitOnce();

    // write even after shutdown so can print the final summaries
    auto writeToConsole = false;
    // ReSharper disable once CppDefaultCaseNotHandledInSwitchStatement
    switch (g_consoleVerbosity) // NOLINT(hicpp-multiway-paths-covered)
    {
        // case 0: // nothing
        // case 1: // status updates
        // case 2: // error info
        case 3: // connection info
        case 4: // connection info + error info
        case 5: // connection info + error info + status updates
        case 6: // above + debug info
        {
            writeToConsole = true;
        }
    }

    if (writeToConsole)
    {
        wprintf_s(
            ProtocolType::TCP == g_configSettings->Protocol ?
            L"[%.3f] TCP connection established [%ws - %ws]\n" :
            L"[%.3f] UDP connection established [%ws - %ws]\n",
            GetStatusTimeStamp(),
            localAddr.WriteCompleteAddress().c_str(),
            remoteAddr.WriteCompleteAddress().c_str());
    }

    if (g_connectionLogger && !g_connectionLogger->IsCsvFormat())
    {
        g_connectionLogger->LogMessage(
            wil::str_printf<std::wstring>(
                ProtocolType::TCP == g_configSettings->Protocol ?
                L"[%.3f] TCP connection established [%ws - %ws]\r\n" :
                L"[%.3f] UDP connection established [%ws - %ws]\r\n",
                GetStatusTimeStamp(),
                localAddr.WriteCompleteAddress().c_str(),
                remoteAddr.WriteCompleteAddress().c_str()).c_str());
    }
}
catch (...)
{
}

void PrintConnectionResults(uint32_t error) noexcept try
{
    ctsConfigInitOnce();

    // write even after shutdown so can print the final summaries
    auto writeToConsole = false;
    // ReSharper disable once CppDefaultCaseNotHandledInSwitchStatement
    switch (g_consoleVerbosity) // NOLINT(hicpp-multiway-paths-covered)
    {
        // case 0: // nothing
        // case 1: // status updates
        // case 2: // error info
        case 3: // connection info
        case 4: // connection info + error info
        case 5: // connection info + error info + status updates
        case 6: // above + debug info
        {
            writeToConsole = true;
        }
    }

    enum class ErrorType
    {
        Success,
        NetworkError,
        ProtocolError
    } errorType = ErrorType::Success;

    if (0 == error)
    {
        errorType = ErrorType::Success;
    }
    else if (ctsIoPattern::IsProtocolError(error))
    {
        errorType = ErrorType::ProtocolError;
    }
    else
    {
        errorType = ErrorType::NetworkError;
    }


    const float currentTime = GetStatusTimeStamp();

    wstring csvString;
    wstring textString;
    wstring errorString;
    if (ErrorType::ProtocolError != errorType)
    {
        if (0 == error)
        {
            errorString = L"Succeeded";
        }
        else
        {
            errorString = wil::str_printf<std::wstring>(
                L"%lu: %ws",
                error,
                ctString::ctFormatMessage(error).c_str());
            // remove any commas from the formatted string - since that will mess up csv files
            ctString::ctReplaceAll(errorString, L",", L" ");
        }
    }

    if (g_connectionLogger && g_connectionLogger->IsCsvFormat())
    {
        // csv format : L"TimeSlice,LocalAddress,RemoteAddress,SendBytes,SendBps,RecvBytes,RecvBps,TimeMs,Result,ConnectionId"
        static const auto* tcpResultCsvFormat = L"%.3f,%ws,%ws,%lld,%lld,%lld,%lld,%lld,%ws,%hs\r\n";
        csvString = wil::str_printf<std::wstring>(
            tcpResultCsvFormat,
            currentTime,
            ctSockaddr().WriteCompleteAddress().c_str(),
            ctSockaddr().WriteCompleteAddress().c_str(),
            0LL,
            0LL,
            0LL,
            0LL,
            0LL,
            errorString.c_str(),
            L"");
    }
    // we'll never write csv format to the console so we'll need a text string in that case
    // - and/or in the case the g_ConnectionLogger isn't writing to csv
    if (writeToConsole || g_connectionLogger && !g_connectionLogger->IsCsvFormat())
    {
        static const auto* tcpNetworkFailureResultTextFormat = L"[%.3f] TCP connection failed with the error %ws : [%ws - %ws] [%hs] : SendBytes[%lld]  SendBps[%lld]  RecvBytes[%lld]  RecvBps[%lld]  Time[%lld ms]";
        textString = wil::str_printf<std::wstring>(
            tcpNetworkFailureResultTextFormat,
            currentTime,
            errorString.c_str(),
            ctSockaddr().WriteCompleteAddress().c_str(),
            ctSockaddr().WriteCompleteAddress().c_str(),
            L"",
            0LL,
            0LL,
            0LL,
            0LL,
            0LL);
    }

    if (writeToConsole)
    {
        // text strings always go to the console
        wprintf(L"%ws\n", textString.c_str());
    }

    if (g_connectionLogger)
    {
        if (g_connectionLogger->IsCsvFormat())
        {
            g_connectionLogger->LogMessage(csvString.c_str());
        }
        else
        {
            g_connectionLogger->LogMessage(
                wil::str_printf<std::wstring>(L"%ws\r\n", textString.c_str()).c_str());
        }
    }
}
catch (...)
{
}

void PrintConnectionResults(const ctSockaddr& localAddr, const ctSockaddr& remoteAddr, uint32_t error, const ctsTcpStatistics& stats) noexcept try
{
    ctsConfigInitOnce();

    // write even after shutdown so can print the final summaries
    auto writeToConsole = false;
    // ReSharper disable once CppDefaultCaseNotHandledInSwitchStatement
    switch (g_consoleVerbosity) // NOLINT(hicpp-multiway-paths-covered)
    {
        // case 0: // nothing
        // case 1: // status updates
        // case 2: // error info
        case 3: // connection info
        case 4: // connection info + error info
        case 5: // connection info + error info + status updates
        case 6: // above + debug info
        {
            writeToConsole = true;
        }
    }

    enum class ErrorType
    {
        Success,
        NetworkError,
        ProtocolError
    } errorType = ErrorType::Success;

    if (0 == error)
    {
        errorType = ErrorType::Success;
    }
    else if (ctsIoPattern::IsProtocolError(error))
    {
        errorType = ErrorType::ProtocolError;
    }
    else
    {
        errorType = ErrorType::NetworkError;
    }

    const int64_t totalTime = stats.m_endTime.GetValue() - stats.m_startTime.GetValue();
    FAIL_FAST_IF_MSG(
        totalTime < 0LL,
        "end_time is less than start_time in this ctsTcpStatistics object (%p)", &stats);
    const float currentTime = GetStatusTimeStamp();

    wstring csvString;
    wstring textString;
    wstring errorString;
    if (ErrorType::ProtocolError != errorType)
    {
        if (0 == error)
        {
            errorString = L"Succeeded";
        }
        else
        {
            errorString = wil::str_printf<std::wstring>(
                L"%lu: %ws",
                error,
                ctString::ctFormatMessage(error).c_str());
            // remove any commas from the formatted string - since that will mess up csv files
            ctString::ctReplaceAll(errorString, L",", L" ");
        }
    }

    if (g_connectionLogger && g_connectionLogger->IsCsvFormat())
    {
        // csv format : L"TimeSlice,LocalAddress,RemoteAddress,SendBytes,SendBps,RecvBytes,RecvBps,TimeMs,Result,ConnectionId"
        static const auto* tcpResultCsvFormat = L"%.3f,%ws,%ws,%lld,%lld,%lld,%lld,%lld,%ws,%hs\r\n";
        csvString = wil::str_printf<std::wstring>(
            tcpResultCsvFormat,
            currentTime,
            localAddr.WriteCompleteAddress().c_str(),
            remoteAddr.WriteCompleteAddress().c_str(),
            stats.m_bytesSent.GetValue(),
            totalTime > 0LL ? stats.m_bytesSent.GetValue() * 1000LL / totalTime : 0LL,
            stats.m_bytesRecv.GetValue(),
            totalTime > 0LL ? stats.m_bytesRecv.GetValue() * 1000LL / totalTime : 0LL,
            totalTime,
            ErrorType::ProtocolError == errorType ?
            ctsIoPattern::BuildProtocolErrorString(error) :
            errorString.c_str(),
            stats.m_connectionIdentifier);
    }
    // we'll never write csv format to the console so we'll need a text string in that case
    // - and/or in the case the g_ConnectionLogger isn't writing to csv
    if (writeToConsole || g_connectionLogger && !g_connectionLogger->IsCsvFormat())
    {
        if (0 == error)
        {
            static const auto* tcpSuccessfulResultTextFormat = L"[%.3f] TCP connection succeeded : [%ws - %ws] [%hs]: SendBytes[%lld]  SendBps[%lld]  RecvBytes[%lld]  RecvBps[%lld]  Time[%lld ms]";
            textString = wil::str_printf<std::wstring>(
                tcpSuccessfulResultTextFormat,
                currentTime,
                localAddr.WriteCompleteAddress().c_str(),
                remoteAddr.WriteCompleteAddress().c_str(),
                stats.m_connectionIdentifier,
                stats.m_bytesSent.GetValue(),
                totalTime > 0LL ? stats.m_bytesSent.GetValue() * 1000LL / totalTime : 0LL,
                stats.m_bytesRecv.GetValue(),
                totalTime > 0LL ? stats.m_bytesRecv.GetValue() * 1000LL / totalTime : 0LL,
                totalTime);
        }
        else
        {
            static const auto* tcpProtocolFailureResultTextFormat = L"[%.3f] TCP connection failed with the protocol error %ws : [%ws - %ws] [%hs] : SendBytes[%lld]  SendBps[%lld]  RecvBytes[%lld]  RecvBps[%lld]  Time[%lld ms]";
            static const auto* tcpNetworkFailureResultTextFormat = L"[%.3f] TCP connection failed with the error %ws : [%ws - %ws] [%hs] : SendBytes[%lld]  SendBps[%lld]  RecvBytes[%lld]  RecvBps[%lld]  Time[%lld ms]";
            textString = wil::str_printf<std::wstring>(
                ErrorType::ProtocolError == errorType ? tcpProtocolFailureResultTextFormat : tcpNetworkFailureResultTextFormat,
                currentTime,
                ErrorType::ProtocolError == errorType ? ctsIoPattern::BuildProtocolErrorString(error) : errorString.c_str(),
                localAddr.WriteCompleteAddress().c_str(),
                remoteAddr.WriteCompleteAddress().c_str(),
                stats.m_connectionIdentifier,
                stats.m_bytesSent.GetValue(),
                totalTime > 0LL ? stats.m_bytesSent.GetValue() * 1000LL / totalTime : 0LL,
                stats.m_bytesRecv.GetValue(),
                totalTime > 0LL ? stats.m_bytesRecv.GetValue() * 1000LL / totalTime : 0LL,
                totalTime);
        }
    }

    if (writeToConsole)
    {
        // text strings always go to the console
        wprintf(L"%ws\n", textString.c_str());
    }

    if (g_connectionLogger)
    {
        if (g_connectionLogger->IsCsvFormat())
        {
            g_connectionLogger->LogMessage(csvString.c_str());
        }
        else
        {
            g_connectionLogger->LogMessage(
                wil::str_printf<std::wstring>(L"%ws\r\n", textString.c_str()).c_str());
        }
    }
}
catch (...)
{
}

void PrintConnectionResults(const ctSockaddr& localAddr, const ctSockaddr& remoteAddr, uint32_t error, const ctsUdpStatistics& stats) noexcept try
{
    ctsConfigInitOnce();

    // write even after shutdown so can print the final summaries
    auto writeToConsole = false;
    // ReSharper disable once CppDefaultCaseNotHandledInSwitchStatement
    switch (g_consoleVerbosity) // NOLINT(hicpp-multiway-paths-covered)
    {
        // case 0: // nothing
        // case 1: // status updates
        // case 2: // error info
        case 3: // connection info
        case 4: // connection info + error info
        case 5: // connection info + error info + status updates
        case 6: // above + debug info
        {
            writeToConsole = true;
        }
    }

    enum class ErrorType
    {
        Success,
        NetworkError,
        ProtocolError
    } errorType{};

    if (0 == error)
    {
        errorType = ErrorType::Success;
    }
    else if (ctsIoPattern::IsProtocolError(error))
    {
        errorType = ErrorType::ProtocolError;
    }
    else
    {
        errorType = ErrorType::NetworkError;
    }

    const float currentTime = GetStatusTimeStamp();
    const int64_t elapsedTime(stats.m_endTime.GetValue() - stats.m_startTime.GetValue());
    const int64_t bitsPerSecond = elapsedTime > 0LL ? stats.m_bitsReceived.GetValue() * 1000LL / elapsedTime : 0LL;

    wstring csvString;
    wstring textString;
    wstring errorString;
    if (ErrorType::ProtocolError != errorType)
    {
        if (0 == error)
        {
            errorString = L"Succeeded";
        }
        else
        {
            errorString = wil::str_printf<std::wstring>(
                L"%lu: %ws",
                error,
                ctString::ctFormatMessage(error).c_str());
            // remove any commas from the formatted string - since that will mess up csv files
            ctString::ctReplaceAll(errorString, L",", L" ");
        }
    }

    if (g_connectionLogger && g_connectionLogger->IsCsvFormat())
    {
        // csv format : "TimeSlice,LocalAddress,RemoteAddress,Bits/Sec,Completed,Dropped,Repeated,Errors,Result,ConnectionId"
        static const auto* udpResultCsvFormat = L"%.3f,%ws,%ws,%llu,%llu,%llu,%llu,%llu,%ws,%hs\r\n";
        csvString = wil::str_printf<std::wstring>(
            udpResultCsvFormat,
            currentTime,
            localAddr.WriteCompleteAddress().c_str(),
            remoteAddr.WriteCompleteAddress().c_str(),
            bitsPerSecond,
            stats.m_successfulFrames.GetValue(),
            stats.m_droppedFrames.GetValue(),
            stats.m_duplicateFrames.GetValue(),
            stats.m_errorFrames.GetValue(),
            ErrorType::ProtocolError == errorType ?
            ctsIoPattern::BuildProtocolErrorString(error) :
            errorString.c_str(),
            stats.m_connectionIdentifier);
    }
    // we'll never write csv format to the console so we'll need a text string in that case
    // - and/or in the case the g_ConnectionLogger isn't writing to csv
    if (writeToConsole || g_connectionLogger && !g_connectionLogger->IsCsvFormat())
    {
        if (0 == error)
        {
            static const auto* udpSuccessfulResultTextFormat = L"[%.3f] UDP connection succeeded : [%ws - %ws] [%hs] : BitsPerSecond [%llu]  Completed [%llu]  Dropped [%llu]  Repeated [%llu]  Errors [%llu]";
            textString = wil::str_printf<std::wstring>(
                udpSuccessfulResultTextFormat,
                currentTime,
                localAddr.WriteCompleteAddress().c_str(),
                remoteAddr.WriteCompleteAddress().c_str(),
                stats.m_connectionIdentifier,
                bitsPerSecond,
                stats.m_successfulFrames.GetValue(),
                stats.m_droppedFrames.GetValue(),
                stats.m_duplicateFrames.GetValue(),
                stats.m_errorFrames.GetValue());
        }
        else
        {
            static const auto* udpProtocolFailureResultTextFormat = L"[%.3f] UDP connection failed with the protocol error %ws : [%ws - %ws] [%hs] : BitsPerSecond [%llu]  Completed [%llu]  Dropped [%llu]  Repeated [%llu]  Errors [%llu]";
            static const auto* udpNetworkFailureResultTextFormat = L"[%.3f] UDP connection failed with the error %ws : [%ws - %ws] [%hs] : BitsPerSecond [%llu]  Completed [%llu]  Dropped [%llu]  Repeated [%llu]  Errors [%llu]";
            textString = wil::str_printf<std::wstring>(
                ErrorType::ProtocolError == errorType ? udpProtocolFailureResultTextFormat : udpNetworkFailureResultTextFormat,
                currentTime,
                ErrorType::ProtocolError == errorType ?
                ctsIoPattern::BuildProtocolErrorString(error) :
                errorString.c_str(),
                localAddr.WriteCompleteAddress().c_str(),
                remoteAddr.WriteCompleteAddress().c_str(),
                stats.m_connectionIdentifier,
                bitsPerSecond,
                stats.m_successfulFrames.GetValue(),
                stats.m_droppedFrames.GetValue(),
                stats.m_duplicateFrames.GetValue(),
                stats.m_errorFrames.GetValue());
        }
    }

    if (writeToConsole)
    {
        // text strings always go to the console
        wprintf(L"%ws\n", textString.c_str());
    }

    if (g_connectionLogger)
    {
        if (g_connectionLogger->IsCsvFormat())
        {
            g_connectionLogger->LogMessage(csvString.c_str());
        }
        else
        {
            g_connectionLogger->LogMessage(
                wil::str_printf<std::wstring>(L"%ws\r\n", textString.c_str()).c_str());
        }
    }
}
catch (...)
{
}

void PrintConnectionResults(const ctSockaddr& localAddr, const ctSockaddr& remoteAddr, uint32_t error) noexcept
{
    if (ProtocolType::TCP == g_configSettings->Protocol)
    {
        PrintConnectionResults(localAddr, remoteAddr, error, ctsTcpStatistics());
    }
    else
    {
        PrintConnectionResults(localAddr, remoteAddr, error, ctsUdpStatistics());
    }
}

void PrintTcpDetails(const ctSockaddr& localAddr, const ctSockaddr& remoteAddr, SOCKET socket, const ctsTcpStatistics& stats) noexcept try
{
    if (g_tcpInfoLogger)
    {
        static const auto* tcpSuccessfulResultTextFormat = L"%.3f, %ws, %ws, %hs, %lld, %lld, %lld, %lld, %lld, ";
        const int64_t totalTime{stats.m_endTime.GetValue() - stats.m_startTime.GetValue()};
        auto textString = wil::str_printf<std::wstring>(
            tcpSuccessfulResultTextFormat,
            GetStatusTimeStamp(),
            localAddr.WriteCompleteAddress().c_str(),
            remoteAddr.WriteCompleteAddress().c_str(),
            stats.m_connectionIdentifier,
            stats.m_bytesSent.GetValue(),
            totalTime > 0LL ? stats.m_bytesSent.GetValue() * 1000LL / totalTime : 0LL,
            stats.m_bytesRecv.GetValue(),
            totalTime > 0LL ? stats.m_bytesRecv.GetValue() * 1000LL / totalTime : 0LL,
            totalTime);

        DWORD bytesReturned;
        TCP_INFO_v1 tcpInfo1{};
        DWORD tcpInfoVersion = 1;
        if (WSAIoctl(
                socket,
                SIO_TCP_INFO,
                &tcpInfoVersion, sizeof tcpInfoVersion,
                &tcpInfo1, sizeof tcpInfo1,
                &bytesReturned,
                nullptr,
                nullptr) == 0)
        {
            // the OS supports TCP_INFO_v1 - write those details
            static const auto* tcpInfoVersion1TextFormat = L"%lu, %lu, %lu, %lu, %lu, %lu, %lu, %lu, %lu, %lu, %lu\r\n";
            textString += wil::str_printf<std::wstring>(
                tcpInfoVersion1TextFormat,
                tcpInfo1.BytesReordered,
                tcpInfo1.BytesRetrans,
                tcpInfo1.SynRetrans,
                tcpInfo1.DupAcksIn,
                tcpInfo1.MinRttUs,
                tcpInfo1.Mss,
                tcpInfo1.TimeoutEpisodes,
                tcpInfo1.FastRetrans,
                tcpInfo1.SndLimBytesCwnd,
                tcpInfo1.SndLimBytesRwin,
                tcpInfo1.SndLimBytesSnd);
            g_tcpInfoLogger->LogMessage(textString.c_str());
            return;
        }

        TCP_INFO_v0 tcpInfo0{};
        tcpInfoVersion = 0;
        if (WSAIoctl(
                socket,
                SIO_TCP_INFO,
                &tcpInfoVersion, sizeof tcpInfoVersion,
                &tcpInfo0, sizeof tcpInfo0,
                &bytesReturned,
                nullptr,
                nullptr) == 0)
        {
            // the OS supports TCP_INFO_v0 - write those details
            static const auto* tcpInfoVersion0TextFormat = L"%lu, %lu, %lu, %lu, %lu, %lu, %lu, %lu";
            textString += wil::str_printf<std::wstring>(
                tcpInfoVersion0TextFormat,
                tcpInfo0.BytesReordered,
                tcpInfo0.BytesRetrans,
                tcpInfo0.SynRetrans,
                tcpInfo0.DupAcksIn,
                tcpInfo0.MinRttUs,
                tcpInfo0.Mss,
                tcpInfo0.TimeoutEpisodes,
                tcpInfo0.FastRetrans);
            g_tcpInfoLogger->LogMessage(textString.c_str());
        }
    }
}
catch (...)
{
}

void __cdecl PrintSummary(_In_z_ _Printf_format_string_ PCWSTR text, ...) noexcept
{
    ctsConfigInitOnce();

    // write even after shutdown so can print the final summaries
    auto writeToConsole = false;
    // ReSharper disable once CppDefaultCaseNotHandledInSwitchStatement
    switch (g_consoleVerbosity) // NOLINT(hicpp-multiway-paths-covered)
    {
        // case 0: // nothing
        case 1: // status updates
        case 2: // error info
        case 3: // connection info
        case 4: // connection info + error info
        case 5: // connection info + error info + status updates
        case 6: // above + debug info
        {
            writeToConsole = true;
        }
    }

    va_list argptr;
    va_start(argptr, text);
    try
    {
        wstring formattedString;
        if (writeToConsole)
        {
            wil::details::str_vprintf_nothrow<std::wstring>(formattedString, text, argptr);
            wprintf(L"%ws", formattedString.c_str());
        }

        if (g_connectionLogger && !g_connectionLogger->IsCsvFormat())
        {
            if (formattedString.empty())
            {
                wil::details::str_vprintf_nothrow<std::wstring>(formattedString, text, argptr);
            }
            g_connectionLogger->LogMessage(
                ctString::ctReplaceAllCopy(
                    formattedString, L"\n", L"\r\n").c_str());
        }
    }
    catch (...)
    {
    }
    va_end(argptr);
}

void PrintErrorInfo(_In_z_ _Printf_format_string_ PCWSTR text, ...) noexcept
{
    ctsConfigInitOnce();

    if (g_shutdownCalled)
    {
        return;
    }

    if (g_breakOnError)
    {
        FAIL_FAST_MSG(ctString::ctConvertToString(text).c_str());
    }

    auto writeToConsole = false;
    // ReSharper disable once CppDefaultCaseNotHandledInSwitchStatement
    switch (g_consoleVerbosity) // NOLINT(hicpp-multiway-paths-covered)
    {
        // case 0: // nothing
        // case 1: // status updates
        case 2: // error info
        // case 3: // connection info
        case 4: // connection info + error info
        case 5: // connection info + error info + status updates
        case 6: // above + debug info
            writeToConsole = true;
            break;
    }

    if (g_errorLogger)
    {
    }

    va_list argptr;
    va_start(argptr, text);
    try
    {
        wstring formattedString;
        if (writeToConsole)
        {
            wil::details::str_vprintf_nothrow<std::wstring>(formattedString, text, argptr);
            wprintf(L"%ws\n", formattedString.c_str());
        }

        if (g_connectionLogger && !g_connectionLogger->IsCsvFormat())
        {
            if (formattedString.empty())
            {
                wil::details::str_vprintf_nothrow<std::wstring>(formattedString, text, argptr);
            }
            g_errorLogger->LogError(
                wil::str_printf<std::wstring>(
                    L"[%.3f] %ws\r\n",
                    GetStatusTimeStamp(),
                    formattedString.c_str()).c_str());
        }
    }
    catch (...)
    {
    }
    va_end(argptr);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
///
/// Get* 
/// - accessor functions made public to retrieve configuration details
///
////////////////////////////////////////////////////////////////////////////////////////////////////
uint32_t GetBufferSize() noexcept
{
    ctsConfigInitOnce();

    return 0 == g_bufferSizeHigh ?
           g_bufferSizeLow :
           g_randomTwister.uniform_int(g_bufferSizeLow, g_bufferSizeHigh);
}

uint32_t GetMaxBufferSize() noexcept
{
    ctsConfigInitOnce();

    return g_bufferSizeHigh == 0 ?
           g_bufferSizeLow :
           g_bufferSizeHigh;
}

uint32_t GetMinBufferSize() noexcept
{
    ctsConfigInitOnce();

    return g_bufferSizeLow;
}


uint64_t GetTransferSize() noexcept
{
    ctsConfigInitOnce();

    return 0 == g_transferSizeHigh ?
           g_transferSizeLow :
           g_randomTwister.uniform_int(g_transferSizeLow, g_transferSizeHigh);
}

int64_t GetTcpBytesPerSecond() noexcept
{
    ctsConfigInitOnce();

    return 0 == g_rateLimitHigh ?
           g_rateLimitLow :
           g_randomTwister.uniform_int(g_rateLimitLow, g_rateLimitHigh);
}

int GetListenBacklog() noexcept
{
    ctsConfigInitOnce();

    auto backlog = SOMAXCONN;
    // Starting in Win8 listen() supports a larger backlog
    if (ctSocketIsRioAvailable())
    {
        backlog = SOMAXCONN_HINT(SOMAXCONN);
    }
    return backlog;
}

const MediaStreamSettings& GetMediaStream() noexcept
{
    ctsConfigInitOnce();

    FAIL_FAST_IF_MSG(
        0 == g_mediaStreamSettings.BitsPerSecond,
        "Internally requesting media stream settings when this was not specified by the user");

    return g_mediaStreamSettings;
}

bool IsListening() noexcept
{
    ctsConfigInitOnce();

    return !g_configSettings->ListenAddresses.empty();
}

float GetStatusTimeStamp() noexcept
{
    return static_cast<float>(ctTimer::SnapQpcInMillis() - g_configSettings->StartTimeMilliseconds) / 1000.0f;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
///
/// Set*Options
/// - functions capturing any options that need to be set on a socket across different states
/// - currently only implementing pre-bind options
///
////////////////////////////////////////////////////////////////////////////////////////////////////
int SetPreBindOptions(SOCKET socket, const ctSockaddr& localAddress) noexcept
{
    ctsConfigInitOnce();

    if (g_configSettings->OutgoingIfIndex > 0)
    {
        constexpr int optlen{sizeof g_configSettings->OutgoingIfIndex};

        if (localAddress.family() == AF_INET)
        {
            // Interface index is in network byte order for IPPROTO_IP.
            const DWORD optionValue = htonl(g_configSettings->OutgoingIfIndex);
            if (setsockopt(
                    socket,
                    IPPROTO_IP, // level
                    IP_UNICAST_IF, // optname
                    reinterpret_cast<const char*>(&optionValue),
                    optlen) != 0)
            {
                const auto gle = WSAGetLastError();
                PrintErrorIfFailed("setsockopt(IP_UNICAST_IF)", gle);
                return gle;
            }
        }
        else
        {
            // Interface index is in host byte order for IPPROTO_IPV6.
            if (setsockopt(
                    socket,
                    IPPROTO_IPV6, // level
                    IPV6_UNICAST_IF, // optname
                    reinterpret_cast<const char*>(&g_configSettings->OutgoingIfIndex),
                    optlen) != 0)
            {
                const auto gle = WSAGetLastError();
                PrintErrorIfFailed("setsockopt(IPV6_UNICAST_IF)", gle);
                return gle;
            }
        }
    }

    //
    // if the user specified bind addresses, enable SO_PORT_SCALABILITY
    // - this will allow each unique IP address the full range of ephemeral ports
    // this option is not available when just binding to INET_ANY (making an ephemeral bind)
    // this option is also not used if the user is binding to an explicit port #
    // - since the port scalability rules no longer apply
    //
    // these only are applicable for outgoing connections
    //
    if (ProtocolType::TCP == g_configSettings->Protocol && !IsListening())
    {
        if (g_configSettings->Options & ReuseUnicastPort)
        {
            // the admin configured the system to use this socket option
            // it is not compatible with SO_PORT_SCALABILITY
            constexpr DWORD optval{1}; // BOOL
            constexpr int optlen{sizeof optval};
#ifndef SO_REUSE_UNICASTPORT
#define SO_REUSE_UNICASTPORT (SO_PORT_SCALABILITY + 1)
#endif
            if (setsockopt(
                    socket,
                    SOL_SOCKET, // level
                    SO_REUSE_UNICASTPORT, // optname
                    reinterpret_cast<const char*>(&optval),
                    optlen) != 0)
            {
                const auto gle = WSAGetLastError();
                PrintErrorIfFailed("setsockopt(SO_REUSE_UNICASTPORT)", gle);
                return gle;
            }
        }
        else if (!localAddress.IsAddressAny() && localAddress.port() == 0)
        {
            constexpr DWORD optval{1}; // BOOL
            // ReSharper disable once CppTooWideScopeInitStatement
            constexpr int optlen{sizeof optval};

            if (setsockopt(
                    socket,
                    SOL_SOCKET, // level
                    SO_PORT_SCALABILITY, // optname
                    reinterpret_cast<const char*>(&optval),
                    optlen) != 0)
            {
                const auto gle = WSAGetLastError();
                PrintErrorIfFailed("setsockopt(SO_PORT_SCALABILITY)", gle);
                return gle;
            }
        }
    }

    if (g_configSettings->Options & LoopbackFastPath)
    {
        DWORD inValue{1};
        DWORD bytesReturned{};

        if (WSAIoctl(
                socket,
                SIO_LOOPBACK_FAST_PATH,
                &inValue, sizeof inValue,
                nullptr, 0,
                &bytesReturned,
                nullptr,
                nullptr) != 0)
        {
            const auto gle = WSAGetLastError();
            PrintErrorIfFailed("WSAIoctl(SIO_LOOPBACK_FAST_PATH)", gle);
            return gle;
        }
    }

    if (g_configSettings->KeepAliveValue > 0)
    {
        tcp_keepalive keepaliveValues{};
        keepaliveValues.onoff = 1;
        keepaliveValues.keepalivetime = g_configSettings->KeepAliveValue;
        keepaliveValues.keepaliveinterval = 1000; // continue to default to 1 second

        DWORD bytesReturned{};
        if (WSAIoctl(
                socket,
                SIO_KEEPALIVE_VALS, // control code
                &keepaliveValues, sizeof keepaliveValues, // in params
                nullptr, 0, // out params
                &bytesReturned,
                nullptr,
                nullptr) != 0)
        {
            const auto gle = WSAGetLastError();
            PrintErrorIfFailed("WSAIoctl(SIO_KEEPALIVE_VALS)", gle);
            return gle;
        }
    }
    else if (g_configSettings->Options & Keepalive)
    {
        constexpr DWORD optval{1};
        // ReSharper disable once CppTooWideScopeInitStatement
        constexpr int optlen{sizeof optval};

        if (setsockopt(
                socket,
                SOL_SOCKET, // level
                SO_KEEPALIVE, // optname
                reinterpret_cast<const char*>(&optval),
                optlen) != 0)
        {
            const auto gle = WSAGetLastError();
            PrintErrorIfFailed("setsockopt(SO_KEEPALIVE)", gle);
            return gle;
        }
    }

    if (g_configSettings->Options & SetRecvBuf)
    {
        const auto recvBuff = g_configSettings->RecvBufValue;
        if (setsockopt(
                socket,
                SOL_SOCKET,
                SO_RCVBUF,
                reinterpret_cast<const char*>(&recvBuff),
                static_cast<int>(sizeof recvBuff)) != 0)
        {
            const auto gle = WSAGetLastError();
            PrintErrorIfFailed("setsockopt(SO_RCVBUF)", gle);
            return gle;
        }
    }

    if (g_configSettings->Options & SetSendBuf)
    {
        const auto sendBuff = g_configSettings->SendBufValue;
        if (setsockopt(
                socket,
                SOL_SOCKET,
                SO_SNDBUF,
                reinterpret_cast<const char*>(&sendBuff),
                static_cast<int>(sizeof sendBuff)) != 0)
        {
            const auto gle = WSAGetLastError();
            PrintErrorIfFailed("setsockopt(SO_SNDBUF)", gle);
            return gle;
        }
    }

    if (g_configSettings->Options & NonBlockingIo)
    {
        u_long enableNonBlocking = 1;
        if (ioctlsocket(socket, FIONBIO, &enableNonBlocking) != 0)
        {
            const auto gle = WSAGetLastError();
            PrintErrorIfFailed("ioctlsocket(FIONBIO)", gle);
            return gle;
        }
    }

    if (g_configSettings->Options & EnableCircularQueueing)
    {
        DWORD bytesReturned{};
        // ReSharper disable once CppTooWideScopeInitStatement
        const auto error = WSAIoctl(
            socket,
            SIO_ENABLE_CIRCULAR_QUEUEING,
            nullptr, 0, // in buffer
            nullptr, 0, // out buffer
            &bytesReturned,
            nullptr,
            nullptr);
        if (error != 0)
        {
            const auto gle = WSAGetLastError();
            PrintErrorIfFailed("WSAIoctl(SIO_ENABLE_CIRCULAR_QUEUEING)", gle);
            return gle;
        }
    }

    if (g_configSettings->Options & HandleInlineIocp)
    {
        if (!SetFileCompletionNotificationModes(reinterpret_cast<HANDLE>(socket), FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)) // NOLINT(performance-no-int-to-ptr)
        {
            const auto gle = GetLastError();
            PrintErrorIfFailed("SetFileCompletionNotificationModes(FILE_SKIP_COMPLETION_PORT_ON_SUCCESS)", gle);
            return static_cast<int>(gle);
        }
    }

    return NO_ERROR;
}

int SetPreConnectOptions(SOCKET) noexcept
{
    ctsConfigInitOnce();
    return 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
///
/// PrintSettings
/// - public function to write out to the console applied settings
///
////////////////////////////////////////////////////////////////////////////////////////////////////
void PrintSettings()
{
    ctsConfigInitOnce();

    wstring settingString(
        L"  Configured Settings  \n"
        L"-----------------------\n");

    settingString.append(L"\tProtocol: ");
    switch (g_configSettings->Protocol)
    {
        case ProtocolType::TCP:
            settingString.append(L"TCP");
            break;
        case ProtocolType::UDP:
            settingString.append(L"UDP");
            break;

        case ProtocolType::NoProtocolSet:
            [[fallthrough]];
        default:
            FAIL_FAST_MSG("Unexpected Settings Protocol");
    }
    settingString.append(L"\n");

    settingString.append(L"\tOptions:");
    if (NoOptionSet == g_configSettings->Options)
    {
        settingString.append(L" None");
    }
    else
    {
        if (g_configSettings->Options & LoopbackFastPath)
        {
            settingString.append(L" TCPFastPath");
        }
        if (g_configSettings->KeepAliveValue > 0)
        {
            settingString.append(L" KeepAlive (");
            settingString.append(std::to_wstring(g_configSettings->KeepAliveValue));
            settingString.append(L")");
        }
        else if (g_configSettings->Options & Keepalive)
        {
            settingString.append(L" KeepAlive");
        }
        if (g_configSettings->Options & NonBlockingIo)
        {
            settingString.append(L" NonBlockingIO");
        }
        if (g_configSettings->Options & HandleInlineIocp)
        {
            settingString.append(L" InlineIOCP");
        }
        if (g_configSettings->Options & ReuseUnicastPort)
        {
            settingString.append(L" ReuseUnicastPort");
        }
        if (g_configSettings->Options & SetRecvBuf)
        {
            settingString.append(wil::str_printf<std::wstring>(L" SO_RCVBUF(%lu)", g_configSettings->RecvBufValue));
        }
        if (g_configSettings->Options & SetSendBuf)
        {
            settingString.append(wil::str_printf<std::wstring>(L" SO_SNDBUF(%lu)", g_configSettings->SendBufValue));
        }
        if (g_configSettings->Options & MsgWaitAll)
        {
            settingString.append(L" MsgWaitAll");
        }
    }
    settingString.append(L"\n");

    settingString.append(wil::str_printf<std::wstring>(L"\tIO function: %ws\n", g_ioFunctionName));

    settingString.append(L"\tIoPattern: ");
    switch (g_configSettings->IoPattern)
    {
        case IoPatternType::Pull:
            settingString.append(L"Pull <TCP client recv/server send>\n");
            break;
        case IoPatternType::Push:
            settingString.append(L"Push <TCP client send/server recv>\n");
            break;
        case IoPatternType::PushPull:
            settingString.append(L"PushPull <TCP client/server alternate send/recv>\n");
            settingString.append(wil::str_printf<std::wstring>(L"\t\tPushBytes: %lu\n", g_configSettings->PushBytes));
            settingString.append(wil::str_printf<std::wstring>(L"\t\tPullBytes: %lu\n", g_configSettings->PullBytes));
            break;
        case IoPatternType::Duplex:
            settingString.append(L"Duplex <TCP client/server both sending and receiving>\n");
            break;
        case IoPatternType::MediaStream:
            settingString.append(L"MediaStream <UDP controlled stream from server to client>\n");
            break;

        case IoPatternType::NoIoSet:
            [[fallthrough]];
        default:
            FAIL_FAST_MSG("Unexpected Settings IoPattern");
    }

    settingString.append(wil::str_printf<std::wstring>(L"\tPrePostRecvs: %u\n", g_configSettings->PrePostRecvs));

    if (g_configSettings->PrePostSends > 0)
    {
        settingString.append(wil::str_printf<std::wstring>(L"\tPrePostSends: %u\n", g_configSettings->PrePostSends));
    }
    else
    {
        settingString.append(wil::str_printf<std::wstring>(L"\tPrePostSends: Following Ideal Send Backlog\n"));
    }

    settingString.append(
        wil::str_printf<std::wstring>(
            L"\tLevel of verification: %ws\n",
            g_configSettings->ShouldVerifyBuffers ? L"Connections & Data" : L"Connections"));

    settingString.append(wil::str_printf<std::wstring>(L"\tPort: %u\n", g_configSettings->Port));

    if (0 == g_bufferSizeHigh)
    {
        settingString.append(
            wil::str_printf<std::wstring>(
                L"\tBuffer used for each IO request: %u [0x%x] bytes\n",
                g_bufferSizeLow, g_bufferSizeLow));
    }
    else
    {
        settingString.append(
            wil::str_printf<std::wstring>(
                L"\tBuffer used for each IO request: [%u, %u] bytes\n",
                g_bufferSizeLow, g_bufferSizeHigh));
    }

    if (0 == g_transferSizeHigh)
    {
        settingString.append(
            wil::str_printf<std::wstring>(
                L"\tTotal transfer per connection: %llu bytes\n",
                g_transferSizeLow));
    }
    else
    {
        settingString.append(
            wil::str_printf<std::wstring>(
                L"\tTotal transfer per connection: [%llu, %llu] bytes\n",
                g_transferSizeLow, g_transferSizeHigh));
    }

    if (ProtocolType::UDP == g_configSettings->Protocol)
    {
        settingString.append(
            wil::str_printf<std::wstring>(
                L"\t\tUDP Stream BitsPerSecond: %lld bits per second\n",
                g_mediaStreamSettings.BitsPerSecond));
        settingString.append(
            wil::str_printf<std::wstring>(
                L"\t\tUDP Stream FrameRate: %lu frames per second\n",
                g_mediaStreamSettings.FramesPerSecond));

        if (g_mediaStreamSettings.BufferDepthSeconds > 0)
        {
            settingString.append(
                wil::str_printf<std::wstring>(
                    L"\t\tUDP Stream BufferDepth: %lu seconds\n",
                    g_mediaStreamSettings.BufferDepthSeconds));
        }

        settingString.append(
            wil::str_printf<std::wstring>(
                L"\t\tUDP Stream StreamLength: %lu seconds (%lu frames)\n",
                g_mediaStreamSettings.StreamLengthSeconds,
                g_mediaStreamSettings.StreamLengthFrames));
        settingString.append(
            wil::str_printf<std::wstring>(
                L"\t\tUDP Stream FrameSize: %lu bytes\n",
                g_mediaStreamSettings.FrameSizeBytes));
    }

    if (ProtocolType::TCP == g_configSettings->Protocol && g_rateLimitLow > 0)
    {
        if (0 == g_rateLimitHigh)
        {
            settingString.append(
                wil::str_printf<std::wstring>(
                    L"\tSending throughput rate limited down to %lld bytes/second\n",
                    g_rateLimitLow));
        }
        else
        {
            settingString.append(
                wil::str_printf<std::wstring>(
                    L"\tSending throughput rate limited down to a range of [%lld, %lld] bytes/second\n",
                    g_rateLimitLow, g_rateLimitHigh));
        }
    }

    if (g_netAdapterAddresses != nullptr)
    {
        settingString.append(
            wil::str_printf<std::wstring>(
                L"\tIP Compartment: %u\n", g_compartmentId));
    }

    if (!g_configSettings->ListenAddresses.empty())
    {
        settingString.append(L"\tAccepting connections on addresses:\n");
        WCHAR wsaddress[SockAddrMaxStringLength]{};
        for (const auto& addr : g_configSettings->ListenAddresses)
        {
            if (addr.WriteCompleteAddress(wsaddress))
            {
                settingString.append(L"\t\t");
                settingString.append(wsaddress);
                settingString.append(L"\n");
            }
        }
    }
    else
    {
        if (g_configSettings->OutgoingIfIndex > 0)
        {
            settingString.append(
                wil::str_printf<std::wstring>(
                    L"\tInterfaceIndex: %u\n", g_configSettings->OutgoingIfIndex));
        }

        settingString.append(L"\tConnecting out to addresses:\n");
        WCHAR wsaddress[SockAddrMaxStringLength]{};
        for (const auto& addr : g_configSettings->TargetAddresses)
        {
            if (addr.WriteCompleteAddress(wsaddress))
            {
                settingString.append(L"\t\t");
                settingString.append(wsaddress);
                settingString.append(L"\n");
            }
        }

        settingString.append(L"\tBinding to local addresses for outgoing connections:\n");
        for (const auto& addr : g_configSettings->BindAddresses)
        {
            if (addr.WriteCompleteAddress(wsaddress))
            {
                settingString.append(L"\t\t");
                settingString.append(wsaddress);
                settingString.append(L"\n");
            }
        }

        if (g_configSettings->LocalPortLow != 0)
        {
            if (0 == g_configSettings->LocalPortHigh)
            {
                settingString.append(
                    wil::str_printf<std::wstring>(
                        L"\tUsing local port for outgoing connections: %u\n",
                        g_configSettings->LocalPortLow));
            }
            else
            {
                settingString.append(
                    wil::str_printf<std::wstring>(
                        L"\tUsing local port for outgoing connections: [%u, %u]\n",
                        g_configSettings->LocalPortLow, g_configSettings->LocalPortHigh));
            }
        }

        settingString.append(
            wil::str_printf<std::wstring>(
                L"\tConnection limit (maximum established connections): %u [0x%x]\n",
                g_configSettings->ConnectionLimit,
                g_configSettings->ConnectionLimit));
        settingString.append(
            wil::str_printf<std::wstring>(
                L"\tConnection throttling rate (maximum pended connection attempts): %u [0x%x]\n",
                g_configSettings->ConnectionThrottleLimit,
                g_configSettings->ConnectionThrottleLimit));
    }
    // calculate total connections
    if (g_configSettings->AcceptFunction)
    {
        if (g_configSettings->ServerExitLimit > MAXLONG)
        {
            settingString.append(
                wil::str_printf<std::wstring>(
                    L"\tServer-accepted connections before exit : 0x%llx\n",
                    g_configSettings->ServerExitLimit));
        }
        else
        {
            settingString.append(
                wil::str_printf<std::wstring>(
                    L"\tServer-accepted connections before exit : %llu [0x%llx]\n",
                    g_configSettings->ServerExitLimit,
                    g_configSettings->ServerExitLimit));
        }
    }
    else
    {
        uint64_t totalConnections{};
        if (g_configSettings->Iterations == MAXULONGLONG)
        {
            totalConnections = MAXULONGLONG;
        }
        else
        {
            totalConnections = g_configSettings->Iterations * g_configSettings->ConnectionLimit;
        }
        if (totalConnections > MAXLONG)
        {
            settingString.append(
                wil::str_printf<std::wstring>(
                    L"\tTotal outgoing connections before exit (iterations * concurrent connections) : 0x%llx\n",
                    totalConnections));
        }
        else
        {
            settingString.append(
                wil::str_printf<std::wstring>(
                    L"\tTotal outgoing connections before exit (iterations * concurrent connections) : %llu [0x%llx]\n",
                    totalConnections,
                    totalConnections));
        }
    }

    settingString.append(L"\n");

    // immediately print the legend once we know the status info object
    switch (g_consoleVerbosity)
    {
        // case 0: // nothing
        case 1: // status updates
        case 2: // error info
        case 3: // error info + status updates
        case 4: // connection info + error info
        case 5: // connection info + error info + status updates
        case 6: // above + debug info
        default:
        {
            fwprintf(stdout, L"%ws", settingString.c_str());
        }
    }

    // must manually convert all carriage returns to file-friendly carriage return/line feed
    if (g_connectionLogger && !g_connectionLogger->IsCsvFormat())
    {
        g_connectionLogger->LogMessage(
            ctString::ctReplaceAllCopy(
                settingString, L"\n", L"\r\n").c_str());
    }
}

SOCKET CreateSocket(int af, int type, int protocol, DWORD dwFlags)
{
    auto oldCompartmentId = NET_IF_COMPARTMENT_ID_UNSPECIFIED;
    auto bCompartmentIdSet = false;

    //
    // g_netAdapterAddresses is created when the user has requested a compartment Id
    // - since we would have had to lookup the interface
    //
    if (g_netAdapterAddresses != nullptr)
    {
        oldCompartmentId = GetCurrentThreadCompartmentId();
        if (oldCompartmentId != g_compartmentId)
        {
            // ReSharper disable once CppTooWideScopeInitStatement
            const auto dwErr = SetCurrentThreadCompartmentId(g_compartmentId);
            if (dwErr != NO_ERROR)
            {
                PrintErrorInfo(wil::str_printf<std::wstring>(L"SetCurrentThreadCompartmentId for ID %u failed err %u", g_compartmentId, dwErr).c_str());
            }
            else
            {
                bCompartmentIdSet = true;
            }
        }
    }

    const auto socket = ::WSASocket(af, type, protocol, nullptr, 0, dwFlags);
    const auto wsaError = WSAGetLastError();

    if (bCompartmentIdSet)
    {
        // ReSharper disable once CppTooWideScopeInitStatement
        const auto dwErr = SetCurrentThreadCompartmentId(oldCompartmentId);
        if (dwErr != NO_ERROR)
        {
            PrintErrorInfo(wil::str_printf<std::wstring>(L"SetCurrentThreadCompartmentId for ID %u failed err %u", oldCompartmentId, dwErr).c_str());
        }
    }

    if (INVALID_SOCKET == socket)
    {
        THROW_WIN32_MSG(wsaError, "WSASocket");
    }

    return socket;
}

bool ShutdownCalled() noexcept
{
    return g_shutdownCalled;
}

uint32_t ConsoleVerbosity() noexcept
{
    return g_consoleVerbosity;
}
} // namespace ctsTraffic
