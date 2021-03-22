#pragma once

#include "bootstrap.h"
#include "config.h"
#include "clickhouse_config.h"
#include "version.h"

#include <yt/yt/ytlib/program/program.h>
#include <yt/yt/ytlib/program/program_config_mixin.h>
#include <yt/yt/ytlib/program/program_pdeathsig_mixin.h>
#include <yt/yt/ytlib/program/program_setsid_mixin.h>
#include <yt/yt/ytlib/program/helpers.h>

#include <yt/yt/library/phdr_cache/phdr_cache.h>

#include <yt/yt/core/ytalloc/bindings.h>

#include <yt/yt/core/misc/ref_counted_tracker_profiler.h>

#include <library/cpp/ytalloc/api/ytalloc.h>

#include <util/system/env.h>
#include <util/system/hostname.h>

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

class TClickHouseServerProgram
    : public TProgram
    , public TProgramPdeathsigMixin
    , public TProgramSetsidMixin
    , public TProgramConfigMixin<TClickHouseServerBootstrapConfig>
{
private:
    TString InstanceId_;
    TString CliqueId_;
    ui16 RpcPort_ = 0;
    ui16 MonitoringPort_ = 0;
    ui16 TcpPort_ = 0;
    ui16 HttpPort_ = 0;

public:
    TClickHouseServerProgram()
        : TProgram(true /* suppressVersion */)
        , TProgramPdeathsigMixin(Opts_)
        , TProgramSetsidMixin(Opts_)
        , TProgramConfigMixin(Opts_)
    {
        Opts_.AddLongOption("instance-id", "ClickHouse instance id")
            .Required()
            .StoreResult(&InstanceId_);
        Opts_.AddLongOption("clique-id", "ClickHouse clique id")
            .Required()
            .StoreResult(&CliqueId_);
        Opts_.AddLongOption("rpc-port", "ytserver RPC port")
            .DefaultValue(9200)
            .StoreResult(&RpcPort_);
        Opts_.AddLongOption("monitoring-port", "ytserver monitoring port")
            .DefaultValue(9201)
            .StoreResult(&MonitoringPort_);
        Opts_.AddLongOption("tcp-port", "ClickHouse TCP port")
            .DefaultValue(9202)
            .StoreResult(&TcpPort_);
        Opts_.AddLongOption("http-port", "ClickHouse HTTP port")
            .DefaultValue(9203)
            .StoreResult(&HttpPort_);
        Opts_.AddLongOption("clickhouse-version", "ClickHouse version")
            .NoArgument()
            .Handler0(std::bind(&TClickHouseServerProgram::PrintClickHouseVersionAndExit, this));
        Opts_.AddLongOption("version", "CHYT version")
            .NoArgument()
            .Handler0(std::bind(&TClickHouseServerProgram::PrintVersionAndExit, this));

        SetCrashOnError();
    }

private:
    virtual void DoRun(const NLastGetopt::TOptsParseResult& parseResult) override
    {
        TThread::SetCurrentThreadName("Main");

        ConfigureUids();
        ConfigureIgnoreSigpipe();
        // NB: ConfigureCrashHandler() is not called intentionally; crash handlers is set up in bootstrap.
        ConfigureExitZeroOnSigterm();
        EnableRefCountedTrackerProfiling();
        NYTAlloc::EnableYTLogging();
        NYTAlloc::EnableYTProfiling();
        NYTAlloc::InitializeLibunwindInterop();
        NYTAlloc::EnableStockpile();
        NYTAlloc::MlockFileMappings();

        if (HandleSetsidOptions()) {
            return;
        }
        if (HandlePdeathsigOptions()) {
            return;
        }

        if (HandleConfigOptions()) {
            return;
        }

        PatchConfigFromEnv();

        auto config = GetConfig();
        auto configNode = GetConfigNode();

        ConfigureSingletons(config);
        StartDiagnosticDump(config);

        // TODO(babenko): This memory leak is intentional.
        // We should avoid destroying bootstrap since some of the subsystems
        // may be holding a reference to it and continue running some actions in background threads.
        auto* bootstrap = new TBootstrap(std::move(config), std::move(configNode));
        bootstrap->Run();
    }

    void PatchConfigFromEnv()
    {
        auto config = GetConfig();

        std::optional<TString> address;
        for (const auto& networkName : {"BB", "BACKBONE", "FASTBONE", "DEFAULT"}) {
            auto addressOrEmpty = GetEnv(Format("YT_IP_ADDRESS_%v", networkName), /*default =*/ "");
            if (!addressOrEmpty.empty()) {
                address = addressOrEmpty;
                break;
            }
        }

        if (address) {
            config->Yt->Address = "[" + *address + "]";
            // In MTN there may be no reasonable FQDN;
            // hostname returns something human-readable, but barely resolvable.
            // COMPAT(max42): move to launcher in future.
            config->AddressResolver->ResolveHostNameIntoFqdn = false;
            HttpPort_ = 10042;
            TcpPort_ = 10043;
            MonitoringPort_ = 10142;
            RpcPort_ = 10143;
        } else {
            config->Yt->Address = GetFQDNHostName();
        }

        config->MonitoringPort = MonitoringPort_;
        config->BusServer->Port = config->RpcPort = RpcPort_;
        config->ClickHouse->TcpPort = TcpPort_;
        config->ClickHouse->HttpPort = HttpPort_;
        config->Yt->CliqueId = TGuid::FromString(CliqueId_);
        config->Yt->InstanceId = TGuid::FromString(InstanceId_);
    }

    void PrintClickHouseVersionAndExit() const
    {
        Cout << VERSION_STRING << Endl;
        _exit(0);
    }

    void PrintVersionAndExit() const
    {
        Cout << GetVersion() << Endl;
        _exit(0);
    }
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer