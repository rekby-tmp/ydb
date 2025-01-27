#include <util/system/platform.h>
#if defined(_linux_) || defined(_darwin_)
#include <ydb/library/yql/udfs/common/clickhouse/client/src/DataTypes/DataTypeArray.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/DataTypes/DataTypeDate.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/DataTypes/DataTypeDateTime64.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/DataTypes/DataTypeEnum.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/DataTypes/DataTypeFactory.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/DataTypes/DataTypeInterval.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/DataTypes/DataTypeNothing.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/DataTypes/DataTypeNullable.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/DataTypes/DataTypeString.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/DataTypes/DataTypeTuple.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/DataTypes/DataTypeUUID.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/DataTypes/DataTypesNumber.h>

#include <ydb/library/yql/udfs/common/clickhouse/client/src/IO/ReadBuffer.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/IO/ReadBufferFromFile.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/Core/Block.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/Core/ColumnsWithTypeAndName.h>

#include <ydb/library/yql/udfs/common/clickhouse/client/src/Formats/FormatFactory.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/Processors/Formats/InputStreamFromInputFormat.h>
#include <ydb/library/yql/udfs/common/clickhouse/client/src/Processors/Formats/Impl/ArrowBufferedStreams.h>

#include <arrow/api.h>
#include <arrow/io/api.h>
#include <arrow/compute/cast.h>
#include <arrow/status.h>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>

#endif

#include "yql_s3_read_actor.h"
#include "yql_s3_source_factory.h"
#include "yql_s3_actors_util.h"

#include <ydb/core/protos/services.pb.h>

#include <ydb/library/yql/minikql/mkql_string_util.h>
#include <ydb/library/yql/minikql/computation/mkql_computation_node_impl.h>
#include <ydb/library/yql/minikql/mkql_program_builder.h>
#include <ydb/library/yql/minikql/invoke_builtins/mkql_builtins.h>
#include <ydb/library/yql/minikql/mkql_function_registry.h>
#include <ydb/library/yql/minikql/mkql_node_cast.h>
#include <ydb/library/yql/minikql/mkql_terminator.h>
#include <ydb/library/yql/minikql/comp_nodes/mkql_factories.h>
#include <ydb/library/yql/providers/common/http_gateway/yql_http_default_retry_policy.h>
#include <ydb/library/yql/providers/common/schema/mkql/yql_mkql_schema.h>
#include <ydb/library/yql/public/udf/arrow/util.h>
#include <ydb/library/yql/utils/yql_panic.h>

#include <ydb/library/yql/providers/s3/common/util.h>
#include <ydb/library/yql/providers/s3/compressors/factory.h>
#include <ydb/library/yql/providers/s3/object_listers/yql_s3_list.h>
#include <ydb/library/yql/providers/s3/proto/range.pb.h>
#include <ydb/library/yql/providers/s3/range_helpers/path_list_reader.h>
#include <ydb/library/yql/providers/s3/serializations/serialization_interval.h>

#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/actor_coroutine.h>
#include <library/cpp/actors/core/events.h>
#include <library/cpp/actors/core/event_local.h>
#include <library/cpp/actors/core/hfunc.h>
#include <library/cpp/actors/core/log.h>

#include <util/generic/size_literals.h>
#include <util/stream/format.h>

#include <queue>

#ifdef THROW
#undef THROW
#endif
#include <library/cpp/xml/document/xml-document.h>


#define LOG_E(name, stream) \
    LOG_ERROR_S(*TlsActivationContext, NKikimrServices::KQP_COMPUTE, name << ": " << this->SelfId() << ", TxId: " << TxId << ". " << stream)
#define LOG_W(name, stream) \
    LOG_WARN_S(*TlsActivationContext, NKikimrServices::KQP_COMPUTE, name << ": " << this->SelfId() << ", TxId: " << TxId << ". " << stream)
#define LOG_I(name, stream) \
    LOG_INFO_S(*TlsActivationContext, NKikimrServices::KQP_COMPUTE, name << ": " << this->SelfId() << ", TxId: " << TxId << ". " << stream)
#define LOG_D(name, stream) \
    LOG_DEBUG_S(*TlsActivationContext, NKikimrServices::KQP_COMPUTE, name << ": " << this->SelfId() << ", TxId: " << TxId << ". " << stream)
#define LOG_T(name, stream) \
    LOG_TRACE_S(*TlsActivationContext, NKikimrServices::KQP_COMPUTE, name << ": " << this->SelfId() << ", TxId: " << TxId << ". " << stream)

#define LOG_CORO_E(name, stream) \
    LOG_ERROR_S(GetActorContext(), NKikimrServices::KQP_COMPUTE, name << ": " << SelfActorId << ", CA: " << ComputeActorId << ", TxId: " << TxId << ". " << stream)
#define LOG_CORO_W(name, stream) \
    LOG_WARN_S(GetActorContext(), NKikimrServices::KQP_COMPUTE, name << ": " << SelfActorId << ", CA: " << ComputeActorId << ", TxId: " << TxId << ". " << stream)
#define LOG_CORO_I(name, stream) \
    LOG_INFO_S(GetActorContext(), NKikimrServices::KQP_COMPUTE, name << ": " << SelfActorId << ", CA: " << ComputeActorId << ", TxId: " << TxId << ". " << stream)
#define LOG_CORO_D(name, stream) \
    LOG_DEBUG_S(GetActorContext(), NKikimrServices::KQP_COMPUTE, name << ": " << SelfActorId << ", CA: " << ComputeActorId << ", TxId: " << TxId << ". " << stream)
#define LOG_CORO_T(name, stream) \
    LOG_TRACE_S(GetActorContext(), NKikimrServices::KQP_COMPUTE, name << ": " << SelfActorId << ", CA: " << ComputeActorId << ", TxId: " << TxId << ". " << stream)

#define THROW_ARROW_NOT_OK(status)                                     \
    do                                                                 \
    {                                                                  \
        if (::arrow::Status _s = (status); !_s.ok())                   \
            throw yexception() << _s.ToString(); \
    } while (false)

namespace NYql::NDq {

using namespace ::NActors;
using namespace ::NYql::NS3Details;

using ::NYql::NS3Lister::ES3PatternVariant;
using ::NYql::NS3Lister::ES3PatternType;

namespace {

constexpr TDuration MEMORY_USAGE_REPORT_PERIOD = TDuration::Seconds(10);

struct TS3ReadAbort : public yexception {
    using yexception::yexception;
};

struct TS3ReadError : public yexception {
    using yexception::yexception;
};

struct TObjectPath {
    TString Path;
    size_t Size;
    size_t PathIndex;

    TObjectPath(TString path, size_t size, size_t pathIndex)
        : Path(std::move(path)), Size(size), PathIndex(pathIndex) { }
};

struct TEvPrivate {
    // Event ids
    enum EEv : ui32 {
        EvBegin = EventSpaceBegin(TEvents::ES_PRIVATE),

        EvReadResult = EvBegin,
        EvDataPart,
        EvReadStarted,
        EvReadFinished,
        EvReadError,
        EvRetry,
        EvNextBlock,
        EvNextRecordBatch,
        EvBlockProcessed,
        EvFileFinished,
        EvPause,
        EvContinue,
        EvFutureResolved,
        EvObjectPathBatch,
        EvObjectPathReadError,

        EvEnd
    };

    static_assert(EvEnd < EventSpaceEnd(TEvents::ES_PRIVATE), "expect EvEnd < EventSpaceEnd(TEvents::ES_PRIVATE)");

    // Events
    struct TEvReadResult : public TEventLocal<TEvReadResult, EvReadResult> {
        TEvReadResult(
            IHTTPGateway::TContent&& result,
            const TString& requestId,
            size_t pathInd,
            TString path)
            : Result(std::move(result))
            , RequestId(requestId)
            , PathIndex(pathInd)
            , Path(std::move(path)) { }

        IHTTPGateway::TContent Result;
        const TString RequestId;
        const size_t PathIndex;
        const TString Path;
    };

    struct TEvDataPart : public TEventLocal<TEvDataPart, EvDataPart> {
        TEvDataPart(IHTTPGateway::TCountedContent&& data) : Result(std::move(data)) {}
        IHTTPGateway::TCountedContent Result;
    };

    struct TEvFutureResolved : public TEventLocal<TEvFutureResolved, EvFutureResolved> {
        TEvFutureResolved() {}
    };

    struct TEvReadStarted : public TEventLocal<TEvReadStarted, EvReadStarted> {
        explicit TEvReadStarted(long httpResponseCode) : HttpResponseCode(httpResponseCode) {}
        const long HttpResponseCode;
    };

    struct TEvReadFinished : public TEventLocal<TEvReadFinished, EvReadFinished> {
        TEvReadFinished(size_t pathIndex, TIssues&& issues)
            : PathIndex(pathIndex), Issues(std::move(issues)) {
        }
        const size_t PathIndex;
        TIssues Issues;
    };

    struct TEvFileFinished : public TEventLocal<TEvFileFinished, EvFileFinished> {
        TEvFileFinished(size_t pathIndex, ui64 ingressDelta)
            : PathIndex(pathIndex), IngressDelta(ingressDelta) {
        }
        const size_t PathIndex;
        ui64 IngressDelta;
    };

    struct TEvReadError : public TEventLocal<TEvReadError, EvReadError> {
        TEvReadError(
            TIssues&& error,
            const TString& requestId,
            size_t pathInd,
            TString path)
            : Error(std::move(error))
            , RequestId(requestId)
            , PathIndex(pathInd)
            , Path(std::move(path)) { }

        const TIssues Error;
        const TString RequestId;
        const size_t PathIndex;
        const TString Path;
    };

    struct TEvRetryEventFunc : public NActors::TEventLocal<TEvRetryEventFunc, EvRetry> {
        explicit TEvRetryEventFunc(std::function<void()> functor) : Functor(std::move(functor)) {}
        const std::function<void()> Functor;
    };

    struct TEvNextBlock : public NActors::TEventLocal<TEvNextBlock, EvNextBlock> {
        TEvNextBlock(NDB::Block& block, size_t pathInd, std::function<void()> functor, ui64 ingressDelta) : PathIndex(pathInd), Functor(functor), IngressDelta(ingressDelta) { Block.swap(block); }
        NDB::Block Block;
        const size_t PathIndex;
        std::function<void()> Functor;
        ui64 IngressDelta;
    };

    struct TEvNextRecordBatch : public NActors::TEventLocal<TEvNextRecordBatch, EvNextRecordBatch> {
        TEvNextRecordBatch(const std::shared_ptr<arrow::RecordBatch>& batch, size_t pathInd, std::function<void()> functor, ui64 ingressDelta) : Batch(batch), PathIndex(pathInd), Functor(functor), IngressDelta(ingressDelta) { }
        std::shared_ptr<arrow::RecordBatch> Batch;
        const size_t PathIndex;
        std::function<void()> Functor;
        ui64 IngressDelta;
    };

    struct TEvBlockProcessed : public NActors::TEventLocal<TEvBlockProcessed, EvBlockProcessed> {
        TEvBlockProcessed() {}
    };

    struct TEvPause : public NActors::TEventLocal<TEvPause, EvPause> {
    };

    struct TEvContinue : public NActors::TEventLocal<TEvContinue, EvContinue> {
    };

    struct TEvObjectPathBatch :
        public NActors::TEventLocal<TEvObjectPathBatch, EvObjectPathBatch> {
        std::vector<TObjectPath> ObjectPaths;
        bool NoMoreFiles = false;
        TEvObjectPathBatch(
            std::vector<TObjectPath> objectPaths, bool noMoreFiles)
            : ObjectPaths(std::move(objectPaths)), NoMoreFiles(noMoreFiles) { }
    };

    struct TEvObjectPathReadError :
        public NActors::TEventLocal<TEvObjectPathReadError, EvObjectPathReadError> {
        TIssues Issues;
        TEvObjectPathReadError(TIssues issues) : Issues(std::move(issues)) { }
    };
};

using namespace NKikimr::NMiniKQL;

class TS3FileQueueActor : public TActorBootstrapped<TS3FileQueueActor> {
public:
    static constexpr char ActorName[] = "YQ_S3_FILE_QUEUE_ACTOR";

    struct TEvPrivatePrivate {
        enum {
            EvGetNextFile = EventSpaceBegin(TEvents::ES_PRIVATE),
            EvNextListingChunkReceived,
            EvEnd
        };
        static_assert(
            EvEnd <= EventSpaceEnd(TEvents::ES_PRIVATE),
            "expected EvEnd <= EventSpaceEnd(TEvents::ES_PRIVATE)");

        struct TEvGetNextFile : public TEventLocal<TEvGetNextFile, EvGetNextFile> {
            size_t RequestedAmount = 1;
            TEvGetNextFile(size_t requestedAmount) : RequestedAmount(requestedAmount){};
        };
        struct TEvNextListingChunkReceived :
            public TEventLocal<TEvNextListingChunkReceived, EvNextListingChunkReceived> {
            NS3Lister::TListResult ListingResult;
            TEvNextListingChunkReceived(NS3Lister::TListResult listingResult)
                : ListingResult(std::move(listingResult)){};
        };
    };
    using TBase = TActorBootstrapped<TS3FileQueueActor>;

    TS3FileQueueActor(
        TTxId  txId,
        TPathList paths,
        size_t prefetchSize,
        ui64 fileSizeLimit,
        IHTTPGateway::TPtr gateway,
        TString url,
        TString token,
        TString pattern,
        ES3PatternVariant patternVariant,
        ES3PatternType patternType)
        : TxId(std::move(txId))
        , PrefetchSize(prefetchSize)
        , FileSizeLimit(fileSizeLimit)
        , MaybeIssues(Nothing())
        , Gateway(std::move(gateway))
        , Url(std::move(url))
        , Token(std::move(token))
        , Pattern(std::move(pattern))
        , PatternVariant(patternVariant)
        , PatternType(patternType) {
        for (size_t i = 0; i < paths.size(); ++i) {
            if (paths[i].IsDirectory) {
                Directories.emplace_back(paths[i].Path, 0, i);
            } else {
                Objects.emplace_back(paths[i].Path, paths[i].Size, i);
            }
        }
    }

    void Bootstrap() {
        if (Directories.empty()) {
            LOG_I("TS3FileQueueActor", "Bootstrap there is no directories to list");
            Become(&TS3FileQueueActor::NoMoreDirectoriesState);
        } else {
            LOG_I("TS3FileQueueActor", "Bootstrap there are directories to list");
            TryPreFetch();
            Become(&TS3FileQueueActor::ThereAreDirectoriesToListState);
        }
    }

    STATEFN(ThereAreDirectoriesToListState) {
        try {
            switch (const auto etype = ev->GetTypeRewrite()) {
                hFunc(TEvPrivatePrivate::TEvGetNextFile, HandleGetNextFile);
                hFunc(TEvPrivatePrivate::TEvNextListingChunkReceived, HandleNextListingChunkReceived);
                cFunc(TEvents::TSystem::Poison, PassAway);
                default:
                    MaybeIssues = TIssues{TIssue{TStringBuilder() << "An event with unknown type has been received: '" << etype << "'"}};
                    TransitToErrorState();
                    break;
            }
        } catch (const std::exception& e) {
            MaybeIssues = TIssues{TIssue{TStringBuilder() << "An unknown exception has occurred: '" << e.what() << "'"}};
            TransitToErrorState();
        }
    }

    void HandleGetNextFile(TEvPrivatePrivate::TEvGetNextFile::TPtr& ev) {
        auto requestAmount = ev->Get()->RequestedAmount;
        LOG_D("TS3FileQueueActor", "HandleGetNextFile requestAmount:" << requestAmount);
        if (Objects.size() > requestAmount) {
            LOG_D("TS3FileQueueActor", "HandleGetNextFile sending right away");
            SendObjects(ev->Sender, requestAmount);
            TryPreFetch();
        } else {
            LOG_D("TS3FileQueueActor", "HandleGetNextFile have not enough objects cached. Start fetching");
            RequestQueue.emplace_back(ev->Sender, requestAmount);
            TryFetch();
        }
    }

    void HandleNextListingChunkReceived(TEvPrivatePrivate::TEvNextListingChunkReceived::TPtr& ev) {
        Y_ENSURE(FetchingInProgress());
        ListingFuture = Nothing();
        LOG_D("TS3FileQueueActor", "HandleNextListingChunkReceived");
        if (SaveRetrievedResults(ev->Get()->ListingResult)) {
            AnswerPendingRequests();
            if (RequestQueue.empty()) {
                LOG_D("TS3FileQueueActor", "HandleNextListingChunkReceived RequestQueue is empty. Trying to prefetch");
                TryPreFetch();
            } else {
                LOG_D("TS3FileQueueActor", "HandleNextListingChunkReceived RequestQueue is not empty. Fetching more objects");
                TryFetch();
            }
        } else {
            TransitToErrorState();
        }
    }

    bool SaveRetrievedResults(const NS3Lister::TListResult& listingResult) {
        LOG_T("TS3FileQueueActor", "SaveRetrievedResults");
        if (std::holds_alternative<TIssues>(listingResult)) {
            MaybeIssues = std::get<TIssues>(listingResult);
            return false;
        }

        auto listingChunk = std::get<NS3Lister::TListEntries>(listingResult);
        LOG_D("TS3FileQueueActor", "SaveRetrievedResults saving: " << listingChunk.Objects.size() << " entries");
        Y_ENSURE(listingChunk.Directories.empty());
        for (auto& object: listingChunk.Objects) {
            if (object.Path.EndsWith('/')) {
                // skip 'directories'
                continue;
            }
            if (object.Size > FileSizeLimit) {
                auto errorMessage = TStringBuilder()
                                    << "Size of object " << object.Path << " = "
                                    << object.Size
                                    << " and exceeds limit = " << FileSizeLimit;
                LOG_E("TS3FileQueueActor", errorMessage);
                MaybeIssues = TIssues{TIssue{errorMessage}};
                return false;
            }
            LOG_T("TS3FileQueueActor", "SaveRetrievedResults adding path: " << object.Path);
            Objects.emplace_back(object.Path, object.Size, CurrentDirectoryPathIndex);
        }
        return true;
    }

    void AnswerPendingRequests() {
        while (!RequestQueue.empty()) {
            auto requestToFulfil = std::find_if(
                RequestQueue.begin(),
                RequestQueue.end(),
                [this](auto& val) { return val.second <= Objects.size(); });

            if (requestToFulfil != RequestQueue.end()) {
                auto [actorId, requestedAmount] = *requestToFulfil;
                LOG_T(
                    "TS3FileQueueActor",
                    "AnswerPendingRequests responding to "
                        << requestToFulfil->first << " with " << requestToFulfil->second
                        << " items");
                SendObjects(actorId, requestedAmount);
                RequestQueue.erase(requestToFulfil);
            } else {
                LOG_T(
                    "TS3FileQueueActor",
                    "AnswerPendingRequests no more pending requests to fulfil");
                break;
            }
        }
    }

    bool FetchingInProgress() const { return ListingFuture.Defined(); }

    void TransitToNoMoreDirectoriesToListState() {
        LOG_I("TS3FileQueueActor", "TransitToNoMoreDirectoriesToListState no more directories to list");
        for (auto& [requestorId, size]: RequestQueue) {
            SendObjects(requestorId, size);
        }
        RequestQueue.clear();
        Become(&TS3FileQueueActor::NoMoreDirectoriesState);
    }

    void TransitToErrorState() {
        Y_ENSURE(MaybeIssues.Defined());
        LOG_I("TS3FileQueueActor", "TransitToErrorState an error occurred sending ");
        for (auto& [requestorId, _]: RequestQueue) {
            Send(
                requestorId,
                std::make_unique<TEvPrivate::TEvObjectPathReadError>(*MaybeIssues));
        }
        RequestQueue.clear();
        Objects.clear();
        Directories.clear();
        Become(&TS3FileQueueActor::AnErrorOccurredState);
    }

    STATEFN(NoMoreDirectoriesState) {
        try {
            switch (const auto etype = ev->GetTypeRewrite()) {
                hFunc(TEvPrivatePrivate::TEvGetNextFile, HandleGetNextFileForEmptyState);
                cFunc(TEvents::TSystem::Poison, PassAway);
                default:
                    MaybeIssues = TIssues{TIssue{TStringBuilder() << "An event with unknown type has been received: '" << etype << "'"}};
                    TransitToErrorState();
                    break;
            }
        } catch (const std::exception& e) {
            MaybeIssues = TIssues{TIssue{TStringBuilder() << "An unknown exception has occurred: '" << e.what() << "'"}};
            TransitToErrorState();
        }
    }

    void HandleGetNextFileForEmptyState(TEvPrivatePrivate::TEvGetNextFile::TPtr& ev) {
        LOG_D("TS3FileQueueActor", "HandleGetNextFileForEmptyState Giving away rest of Objects");
        SendObjects(ev->Sender, ev->Get()->RequestedAmount);
    }

    STATEFN(AnErrorOccurredState) {
        try {
            switch (const auto etype = ev->GetTypeRewrite()) {
                hFunc(TEvPrivatePrivate::TEvGetNextFile, HandleGetNextFileForErrorState);
                cFunc(TEvents::TSystem::Poison, PassAway);
                default:
                    MaybeIssues = TIssues{TIssue{TStringBuilder() << "An event with unknown type has been received: '" << etype << "'"}};
                    break;
            }
        } catch (const std::exception& e) {
            MaybeIssues = TIssues{TIssue{TStringBuilder() << "An unknown exception has occurred: '" << e.what() << "'"}};
        }
    }

    void HandleGetNextFileForErrorState(TEvPrivatePrivate::TEvGetNextFile::TPtr& ev) {
        LOG_D(
            "TS3FileQueueActor",
            "HandleGetNextFileForErrorState Giving away rest of Objects");
        Send(ev->Sender, std::make_unique<TEvPrivate::TEvObjectPathReadError>(*MaybeIssues));
    }

    void PassAway() override {
        if (!MaybeIssues.Defined()) {
            for (auto& [requestorId, size]: RequestQueue) {
                SendObjects(requestorId, size);
            }
        } else {
            for (auto& [requestorId, _]: RequestQueue) {
                Send(
                    requestorId,
                    std::make_unique<TEvPrivate::TEvObjectPathReadError>(*MaybeIssues));
            }
        }

        RequestQueue.clear();
        Objects.clear();
        Directories.clear();
        TBase::PassAway();
    }

private:
    void SendObjects(const TActorId& recipient, size_t amount) {
        Y_ENSURE(!MaybeIssues.Defined());
        size_t correctedAmount = std::min(amount, Objects.size());
        std::vector<TObjectPath> result;
        if (correctedAmount != 0) {
            result.reserve(correctedAmount);
            for (size_t i = 0; i < correctedAmount; ++i) {
                result.push_back(Objects.back());
                Objects.pop_back();
            }
        }

        LOG_T(
            "TS3FileQueueActor",
            "SendObjects amount: " << amount << " correctedAmount: " << correctedAmount
                                   << " result size: " << result.size());

        Send(
            recipient,
            std::make_unique<TEvPrivate::TEvObjectPathBatch>(
                std::move(result), HasNoMoreItems()));
    }
    bool HasNoMoreItems() const {
        return !(MaybeLister.Defined() && (*MaybeLister)->HasNext()) &&
               Directories.empty() && Objects.empty();
    }

    bool TryPreFetch () {
        if (Objects.size() < PrefetchSize) {
            return TryFetch();
        }
        return false;
    }
    bool TryFetch() {
        if (FetchingInProgress()) {
            LOG_D("TS3FileQueueActor", "TryFetch fetching already in progress");
            return true;
        }

        if (MaybeLister.Defined() && (*MaybeLister)->HasNext()) {
            LOG_D("TS3FileQueueActor", "TryFetch fetching from current lister");
            Fetch();
            return true;
        }

        if (!Directories.empty()) {
            LOG_D("TS3FileQueueActor", "TryFetch fetching from new lister");

            auto [path, size, pathIndex] = Directories.back();
            Directories.pop_back();
            CurrentDirectoryPathIndex = pathIndex;
            MaybeLister = NS3Lister::MakeS3Lister(
                Gateway,
                NS3Lister::TListingRequest{
                    Url,
                    Token,
                    PatternVariant == ES3PatternVariant::PathPattern
                        ? Pattern
                        : TStringBuilder{} << path << Pattern,
                    PatternType,
                    path},
                Nothing(),
                false);
            Fetch();
            return true;
        }

        LOG_D("TS3FileQueueActor", "TryFetch couldn't start fetching");
        MaybeLister = Nothing();
        TransitToNoMoreDirectoriesToListState();
        return false;
    }
    void Fetch() {
        Y_ENSURE(!ListingFuture.Defined());
        Y_ENSURE(MaybeLister.Defined());
        NActors::TActorSystem* actorSystem = NActors::TActivationContext::ActorSystem();
        ListingFuture =
            (*MaybeLister)
                ->Next()
                .Subscribe([actorSystem, selfId = SelfId()](
                               const NThreading::TFuture<NS3Lister::TListResult>& future) {
                    actorSystem->Send(
                        selfId,
                        new TEvPrivatePrivate::TEvNextListingChunkReceived(
                            future.GetValue()));
                });
    }

private:
    const TTxId TxId;

    std::vector<TObjectPath> Objects;
    std::vector<TObjectPath> Directories;

    size_t PrefetchSize;
    ui64 FileSizeLimit;
    TMaybe<NS3Lister::IS3Lister::TPtr> MaybeLister = Nothing();
    TMaybe<NThreading::TFuture<NS3Lister::TListResult>> ListingFuture;
    size_t CurrentDirectoryPathIndex = 0;
    std::deque<std::pair<TActorId, size_t>> RequestQueue;
    TMaybe<TIssues> MaybeIssues;

    const IHTTPGateway::TPtr Gateway;
    const TString Url;
    const TString Token;
    const TString Pattern;
    const ES3PatternVariant PatternVariant;
    const ES3PatternType PatternType;
};

class TS3ReadActor : public TActorBootstrapped<TS3ReadActor>, public IDqComputeActorAsyncInput {
public:
    TS3ReadActor(ui64 inputIndex,
        const TTxId& txId,
        IHTTPGateway::TPtr gateway,
        const THolderFactory& holderFactory,
        const TString& url,
        const TString& token,
        const TString& pattern,
        ES3PatternVariant patternVariant,
        TPathList&& paths,
        bool addPathIndex,
        ui64 startPathIndex,
        const NActors::TActorId& computeActorId,
        ui64 sizeLimit,
        const IRetryPolicy<long>::TPtr& retryPolicy,
        const TS3ReadActorFactoryConfig& readActorFactoryCfg,
        ::NMonitoring::TDynamicCounterPtr counters,
        ::NMonitoring::TDynamicCounterPtr taskCounters,
        ui64 fileSizeLimit)
        : ReadActorFactoryCfg(readActorFactoryCfg)
        , Gateway(std::move(gateway))
        , HolderFactory(holderFactory)
        , InputIndex(inputIndex)
        , TxId(txId)
        , ComputeActorId(computeActorId)
        , RetryPolicy(retryPolicy)
        , ActorSystem(TActivationContext::ActorSystem())
        , Url(url)
        , Token(token)
        , Pattern(pattern)
        , PatternVariant(patternVariant)
        , Paths(std::move(paths))
        , AddPathIndex(addPathIndex)
        , StartPathIndex(startPathIndex)
        , SizeLimit(sizeLimit)
        , Counters(counters)
        , TaskCounters(taskCounters)
        , FileSizeLimit(fileSizeLimit) {
        if (Counters) {
            QueueDataSize = Counters->GetCounter("QueueDataSize");
            QueueDataLimit = Counters->GetCounter("QueueDataLimit");
            QueueBlockCount = Counters->GetCounter("QueueBlockCount");
            QueueDataLimit->Add(ReadActorFactoryCfg.DataInflight);
        }
        if (TaskCounters) {
            TaskQueueDataSize = TaskCounters->GetCounter("QueueDataSize");
            TaskQueueDataLimit = TaskCounters->GetCounter("QueueDataLimit");
            TaskQueueDataLimit->Add(ReadActorFactoryCfg.DataInflight);
        }
    }

    void Bootstrap() {
        LOG_D("TS3ReadActor", "Bootstrap" << ", InputIndex: " << InputIndex);
        FileQueueActor = RegisterWithSameMailbox(new TS3FileQueueActor{
            TxId,
            std::move(Paths),
            ReadActorFactoryCfg.MaxInflight * 2,
            FileSizeLimit,
            Gateway,
            Url,
            Token,
            Pattern,
            PatternVariant,
            ES3PatternType::Wildcard});
        SendPathRequest();
        Become(&TS3ReadActor::StateFunc);
    }

    bool TryStartDownload() {
        if (ObjectPathCache.empty()) {
            // no path is pending
            return false;
        }
        if (QueueTotalDataSize > ReadActorFactoryCfg.DataInflight) {
            // too large data inflight
            return false;
        }
        if (DownloadInflight >= ReadActorFactoryCfg.MaxInflight) {
            // too large download inflight
            return false;
        }

        StartDownload();
        return true;
    }

    void StartDownload() {
        DownloadInflight++;
        const auto& [path, size, index] = ReadPathFromCache();
        auto url = Url + path;
        auto id = index + StartPathIndex;
        const TString requestId = CreateGuidAsString();
        LOG_D("TS3ReadActor", "Download: " << url << ", ID: " << id << ", request id: [" << requestId << "]");
        Gateway->Download(url, MakeHeaders(Token, requestId), 0U, std::min(size, SizeLimit),
            std::bind(&TS3ReadActor::OnDownloadFinished, ActorSystem, SelfId(), requestId, std::placeholders::_1, id, path), {}, RetryPolicy);
    }

    TObjectPath ReadPathFromCache() {
        Y_ENSURE(!ObjectPathCache.empty());
        auto object = ObjectPathCache.back();
        ObjectPathCache.pop_back();
        if (ObjectPathCache.empty() && !IsObjectQueueEmpty) {
            SendPathRequest();
        }
        return object;
    }
    void SendPathRequest() {
        Y_ENSURE(!IsWaitingObjectQueueResponse);
        Send(
            FileQueueActor,
            std::make_unique<TS3FileQueueActor::TEvPrivatePrivate::TEvGetNextFile>(
                ReadActorFactoryCfg.MaxInflight));
        IsWaitingObjectQueueResponse = true;
    }

    static constexpr char ActorName[] = "S3_READ_ACTOR";

private:
    void SaveState(const NDqProto::TCheckpoint&, NDqProto::TSourceState&) final {}
    void LoadState(const NDqProto::TSourceState&) final {}
    void CommitState(const NDqProto::TCheckpoint&) final {}
    ui64 GetInputIndex() const final { return InputIndex; }

    ui64 GetIngressBytes() override {
        return IngressBytes;
    }

    STRICT_STFUNC(StateFunc,
        hFunc(TEvPrivate::TEvReadResult, Handle);
        hFunc(TEvPrivate::TEvReadError, Handle);
        hFunc(TEvPrivate::TEvObjectPathBatch, HandleObjectPathBatch);
        hFunc(TEvPrivate::TEvObjectPathReadError, HandleObjectPathReadError);
    )

    void HandleObjectPathBatch(TEvPrivate::TEvObjectPathBatch::TPtr& objectPathBatch) {
        Y_ENSURE(IsWaitingObjectQueueResponse);
        IsWaitingObjectQueueResponse = false;
        ListedFiles += objectPathBatch->Get()->ObjectPaths.size();
        IsObjectQueueEmpty = objectPathBatch->Get()->NoMoreFiles;
        ObjectPathCache.insert(
            ObjectPathCache.end(),
            std::make_move_iterator(objectPathBatch->Get()->ObjectPaths.begin()),
            std::make_move_iterator(objectPathBatch->Get()->ObjectPaths.end()));
        while (TryStartDownload()) {}
    }
    void HandleObjectPathReadError(TEvPrivate::TEvObjectPathReadError::TPtr& result) {
        IsObjectQueueEmpty = true;
        LOG_E("TS3ReadActor", "Error while object listing, details: TEvObjectPathReadError: " << result->Get()->Issues.ToOneLineString());
        auto issues = NS3Util::AddParentIssue(TStringBuilder{} << "Error while object listing", TIssues{result->Get()->Issues});
        Send(ComputeActorId, new TEvAsyncInputError(InputIndex, issues, NYql::NDqProto::StatusIds::EXTERNAL_ERROR));
    }

    static void OnDownloadFinished(TActorSystem* actorSystem, TActorId selfId, const TString& requestId, IHTTPGateway::TResult&& result, size_t pathInd, const TString path) {
        switch (result.index()) {
        case 0U:
            actorSystem->Send(new IEventHandle(selfId, TActorId(), new TEvPrivate::TEvReadResult(std::get<IHTTPGateway::TContent>(std::move(result)), requestId, pathInd, path)));
            return;
        case 1U:
            actorSystem->Send(new IEventHandle(selfId, TActorId(), new TEvPrivate::TEvReadError(std::get<TIssues>(std::move(result)), requestId, pathInd, path)));
            return;
        default:
            break;
        }
    }

    i64 GetAsyncInputData(TUnboxedValueVector& buffer, TMaybe<TInstant>&, bool& finished, i64 freeSpace) final {
        i64 total = 0LL;
        if (!Blocks.empty()) {
            buffer.reserve(buffer.size() + Blocks.size());
            do {
                auto& content = std::get<IHTTPGateway::TContent>(Blocks.front());
                const auto size = content.size();
                auto value = MakeString(std::string_view(content));
                if (AddPathIndex) {
                    NUdf::TUnboxedValue* tupleItems = nullptr;
                    auto tuple = ContainerCache.NewArray(HolderFactory, 2, tupleItems);
                    *tupleItems++ = value;
                    *tupleItems++ = NUdf::TUnboxedValuePod(std::get<ui64>(Blocks.front()));
                    value = tuple;
                }

                buffer.emplace_back(std::move(value));
                Blocks.pop();
                total += size;
                freeSpace -= size;

                QueueTotalDataSize -= size;
                if (Counters) {
                    QueueDataSize->Sub(size);
                    QueueBlockCount->Dec();
                }
                if (TaskCounters) {
                    TaskQueueDataSize->Sub(size);
                }
                TryStartDownload();
            } while (!Blocks.empty() && freeSpace > 0LL);
        }

        if (LastFileWasProcessed()) {
            finished = true;
            ContainerCache.Clear();
        }

        return total;
    }
    bool LastFileWasProcessed() const {
        return Blocks.empty() && (ListedFiles == CompletedFiles) && IsObjectQueueEmpty;
    }

    void Handle(TEvPrivate::TEvReadResult::TPtr& result) {
        ++CompletedFiles;
        const auto id = result->Get()->PathIndex;
        const auto path = result->Get()->Path;
        const auto httpCode = result->Get()->Result.HttpResponseCode;
        const auto requestId = result->Get()->RequestId;
        IngressBytes += result->Get()->Result.size();
        LOG_D("TS3ReadActor", "ID: " << id << ", Path: " << path << ", read size: " << result->Get()->Result.size() << ", HTTP response code: " << httpCode << ", request id: [" << requestId << "]");
        if (200 == httpCode || 206 == httpCode) {
            auto size = result->Get()->Result.size();
            QueueTotalDataSize += size;
            if (Counters) {
                QueueBlockCount->Inc();
                QueueDataSize->Add(size);
            }
            if (TaskCounters) {
                TaskQueueDataSize->Add(size);
            }
            Blocks.emplace(std::make_tuple(std::move(result->Get()->Result), id));
            DownloadInflight--;
            TryStartDownload();
            Send(ComputeActorId, new TEvNewAsyncInputDataArrived(InputIndex));
        } else {
            TString errorText = result->Get()->Result.Extract();
            TString errorCode;
            TString message;
            if (!ParseS3ErrorResponse(errorText, errorCode, message)) {
                message = errorText;
            }
            message = TStringBuilder{} << "Error while reading file " << path << ", details: " << message << ", request id: [" << requestId << "]";
            Send(ComputeActorId, new TEvAsyncInputError(InputIndex, BuildIssues(httpCode, errorCode, message), NYql::NDqProto::StatusIds::EXTERNAL_ERROR));
        }
    }

    void Handle(TEvPrivate::TEvReadError::TPtr& result) {
        ++CompletedFiles;
        auto id = result->Get()->PathIndex;
        const auto requestId = result->Get()->RequestId;
        const auto path = result->Get()->Path;
        LOG_W("TS3ReadActor", "Error while reading file " << path << ", details: ID: " << id << ", TEvReadError: " << result->Get()->Error.ToOneLineString() << ", request id: [" << requestId << "]");
        auto issues = NS3Util::AddParentIssue(TStringBuilder{} << "Error while reading file " << path << " with request id [" << requestId << "]", TIssues{result->Get()->Error});
        Send(ComputeActorId, new TEvAsyncInputError(InputIndex, std::move(issues), NYql::NDqProto::StatusIds::EXTERNAL_ERROR));
    }

    // IActor & IDqComputeActorAsyncInput
    void PassAway() override { // Is called from Compute Actor
        LOG_D("TS3ReadActor", "PassAway");

        if (Counters) {
            QueueDataSize->Sub(QueueTotalDataSize);
            QueueBlockCount->Sub(Blocks.size());
            QueueDataLimit->Sub(ReadActorFactoryCfg.DataInflight);
        }
        if (TaskCounters) {
            TaskQueueDataSize->Sub(QueueTotalDataSize);
            TaskQueueDataLimit->Sub(ReadActorFactoryCfg.DataInflight);
        }
        QueueTotalDataSize = 0;

        ContainerCache.Clear();
        Send(FileQueueActor, new NActors::TEvents::TEvPoison());
        TActorBootstrapped<TS3ReadActor>::PassAway();
    }

    static IHTTPGateway::THeaders MakeHeaders(const TString& token, const TString& requestId) {
        IHTTPGateway::THeaders headers{TString{"X-Request-ID:"} += requestId};
        if (token) {
            headers.emplace_back(TString("X-YaCloud-SubjectToken:") += token);
        }
        return headers;
    }

private:
    const TS3ReadActorFactoryConfig ReadActorFactoryCfg;
    const IHTTPGateway::TPtr Gateway;
    const THolderFactory& HolderFactory;
    TPlainContainerCache ContainerCache;

    const ui64 InputIndex;
    const TTxId TxId;
    const NActors::TActorId ComputeActorId;
    const IRetryPolicy<long>::TPtr RetryPolicy;

    TActorSystem* const ActorSystem;

    const TString Url;
    const TString Token;
    const TString Pattern;
    const ES3PatternVariant PatternVariant;
    TPathList Paths;
    std::vector<TObjectPath> ObjectPathCache;
    bool IsObjectQueueEmpty = false;
    bool IsWaitingObjectQueueResponse = false;
    size_t ListedFiles = 0;
    size_t CompletedFiles = 0;
    NActors::TActorId FileQueueActor;
    const bool AddPathIndex;
    const ui64 StartPathIndex;
    const ui64 SizeLimit;
    ui64 IngressBytes = 0;

    std::queue<std::tuple<IHTTPGateway::TContent, ui64>> Blocks;

    ::NMonitoring::TDynamicCounters::TCounterPtr QueueDataSize;
    ::NMonitoring::TDynamicCounters::TCounterPtr QueueDataLimit;
    ::NMonitoring::TDynamicCounters::TCounterPtr QueueBlockCount;
    ::NMonitoring::TDynamicCounters::TCounterPtr DownloadPaused;
    ::NMonitoring::TDynamicCounters::TCounterPtr DeferredQueueSize;
    ::NMonitoring::TDynamicCounters::TCounterPtr TaskDownloadPaused;
    ::NMonitoring::TDynamicCounters::TCounterPtr TaskQueueDataSize;
    ::NMonitoring::TDynamicCounters::TCounterPtr TaskQueueDataLimit;
    ::NMonitoring::TDynamicCounterPtr Counters;
    ::NMonitoring::TDynamicCounterPtr TaskCounters;
    ui64 QueueTotalDataSize = 0;
    ui64 DownloadInflight = 0;
    const ui64 FileSizeLimit;
};

struct TReadSpec {
    using TPtr = std::shared_ptr<TReadSpec>;

    bool Arrow = false;
    NDB::ColumnsWithTypeAndName CHColumns;
    std::shared_ptr<arrow::Schema> ArrowSchema;
    NDB::FormatSettings Settings;
    TString Format, Compression;
    ui64 SizeLimit = 0;
    ui32 BlockLengthPosition = 0;
    std::vector<ui32> ColumnReorder;
};

struct TRetryStuff {
    using TPtr = std::shared_ptr<TRetryStuff>;

    TRetryStuff(
        IHTTPGateway::TPtr gateway,
        TString url,
        const IHTTPGateway::THeaders& headers,
        std::size_t sizeLimit,
        const TTxId& txId,
        const TString& requestId,
        const IRetryPolicy<long>::TPtr& retryPolicy
    ) : Gateway(std::move(gateway))
      , Url(std::move(url))
      , Headers(headers)
      , Offset(0U)
      , SizeLimit(sizeLimit)
      , TxId(txId)
      , RequestId(requestId)
      , RetryState(retryPolicy->CreateRetryState())
      , RetryPolicy(retryPolicy)
      , Cancelled(false)
    {}

    const IHTTPGateway::TPtr Gateway;
    const TString Url;
    const IHTTPGateway::THeaders Headers;
    std::size_t Offset, SizeLimit;
    const TTxId TxId;
    const TString RequestId;
    IRetryPolicy<long>::IRetryState::TPtr RetryState;
    IRetryPolicy<long>::TPtr RetryPolicy;
    IHTTPGateway::TCancelHook CancelHook;
    TMaybe<TDuration> NextRetryDelay;
    std::atomic_bool Cancelled;

    const IRetryPolicy<long>::IRetryState::TPtr& GetRetryState() {
        if (!RetryState) {
            RetryState = RetryPolicy->CreateRetryState();
        }
        return RetryState;
    }

    void Cancel() {
        Cancelled.store(true);
        if (const auto cancelHook = std::move(CancelHook)) {
            CancelHook = {};
            cancelHook(TIssue("Request cancelled."));
        }
    }

    bool IsCancelled() {
        return Cancelled.load();
    }
};

void OnDownloadStart(TActorSystem* actorSystem, const TActorId& self, const TActorId& parent, long httpResponseCode) {
    actorSystem->Send(new IEventHandle(self, parent, new TEvPrivate::TEvReadStarted(httpResponseCode)));
}

void OnNewData(TActorSystem* actorSystem, const TActorId& self, const TActorId& parent, IHTTPGateway::TCountedContent&& data) {
    actorSystem->Send(new IEventHandle(self, parent, new TEvPrivate::TEvDataPart(std::move(data))));
}

void OnDownloadFinished(TActorSystem* actorSystem, const TActorId& self, const TActorId& parent, size_t pathIndex, TIssues issues) {
    actorSystem->Send(new IEventHandle(self, parent, new TEvPrivate::TEvReadFinished(pathIndex, std::move(issues))));
}

void DownloadStart(const TRetryStuff::TPtr& retryStuff, TActorSystem* actorSystem, const TActorId& self, const TActorId& parent, size_t pathIndex, const ::NMonitoring::TDynamicCounters::TCounterPtr& inflightCounter) {
    retryStuff->CancelHook = retryStuff->Gateway->Download(retryStuff->Url,
        retryStuff->Headers, retryStuff->Offset, retryStuff->SizeLimit,
        std::bind(&OnDownloadStart, actorSystem, self, parent, std::placeholders::_1),
        std::bind(&OnNewData, actorSystem, self, parent, std::placeholders::_1),
        std::bind(&OnDownloadFinished, actorSystem, self, parent, pathIndex, std::placeholders::_1),
        inflightCounter);
}

template <typename T>
class IBatchReader {
public:
    virtual ~IBatchReader() = default;

    virtual bool Next(T& value) = 0;
};

class TBlockReader : public IBatchReader<NDB::Block> {
public:
    TBlockReader(std::unique_ptr<NDB::IBlockInputStream>&& stream)
        : Stream(std::move(stream))
    {}

    bool Next(NDB::Block& value) final {
        value = Stream->read();
        return !!value;
    }

private:
    std::unique_ptr<NDB::IBlockInputStream> Stream;
};

using TColumnConverter = std::function<std::shared_ptr<arrow::Array>(const std::shared_ptr<arrow::Array>&)>;

class TArrowParquetBatchReader : public IBatchReader<std::shared_ptr<arrow::RecordBatch>> {
public:
    TArrowParquetBatchReader(TArrowFileDesc&& fileDesc, IArrowReader::TPtr arrowReader, int numRowGroups, std::vector<int>&& columnIndices, std::vector<TColumnConverter>&& columnConverters, std::function<void()>&& onFutureResolve, std::function<void()>&& waitForFutureResolve)
        : FileDesc(std::move(fileDesc))
        , ArrowReader(arrowReader)
        , ColumnIndices(std::move(columnIndices))
        , ColumnConverters(std::move(columnConverters))
        , TotalGroups(numRowGroups)
        , CurrentGroup(0)
        , OnFutureResolve(std::move(onFutureResolve))
        , WaitForFutureResolve(std::move(waitForFutureResolve))
    {}

    bool Next(std::shared_ptr<arrow::RecordBatch>& value) final {
        for (;;) {
            if (CurrentGroup == TotalGroups) {
                return false;
            }

            if (!CurrentBatchReader) {
                auto future = ArrowReader->ReadRowGroup(FileDesc, CurrentGroup++, ColumnIndices);
                future.Subscribe([this](const NThreading::TFuture<std::shared_ptr<arrow::Table>>&){
                                    OnFutureResolve();
                                });
                WaitForFutureResolve();
                CurrentTable = future.GetValue();
                CurrentBatchReader = std::make_unique<arrow::TableBatchReader>(*CurrentTable);
            }

            THROW_ARROW_NOT_OK(CurrentBatchReader->ReadNext(&value));
            if (value) {
                auto columns = value->columns();
                for (size_t i = 0; i < ColumnConverters.size(); ++i) {
                    auto converter = ColumnConverters[i];
                    if (converter) {
                        columns[i] = converter(columns[i]);
                    }
                }

                value = arrow::RecordBatch::Make(value->schema(), value->num_rows(), columns);
                return true;
            }

            CurrentBatchReader = nullptr;
            CurrentTable = nullptr;
        }
    }

private:
    TArrowFileDesc FileDesc;
    IArrowReader::TPtr ArrowReader;
    const std::vector<int> ColumnIndices;
    std::vector<TColumnConverter> ColumnConverters;
    const int TotalGroups;
    int CurrentGroup;
    std::shared_ptr<arrow::Table> CurrentTable;
    std::unique_ptr<arrow::TableBatchReader> CurrentBatchReader;
    std::function<void()> OnFutureResolve;
    std::function<void()> WaitForFutureResolve;
};

class TS3ReadCoroImpl : public TActorCoroImpl {
    friend class TS3StreamReadActor;
private:
    class TReadBufferFromStream : public NDB::ReadBuffer {
    public:
        TReadBufferFromStream(TS3ReadCoroImpl* coro)
            : NDB::ReadBuffer(nullptr, 0ULL), Coro(coro)
        {}
    private:
        bool nextImpl() final {
            while (Coro->Next(Value)) {
                if (!Value.empty()) {
                    working_buffer = NDB::BufferBase::Buffer(const_cast<char*>(Value.data()), const_cast<char*>(Value.data()) + Value.size());
                    return true;
                }
            }
            return false;
        }

        TS3ReadCoroImpl *const Coro;
        TString Value;
    };

    static constexpr std::string_view TruncatedSuffix = "... [truncated]"sv;
public:
    TS3ReadCoroImpl(ui64 inputIndex, const TTxId& txId, const NActors::TActorId& computeActorId,
        const TRetryStuff::TPtr& retryStuff, const TReadSpec::TPtr& readSpec, size_t pathIndex,
        const TString& path, const TString& url, const std::size_t maxBlocksInFly, IArrowReader::TPtr arrowReader,
        const TS3ReadActorFactoryConfig& readActorFactoryCfg,
        const ::NMonitoring::TDynamicCounters::TCounterPtr& deferredQueueSize,
        const ::NMonitoring::TDynamicCounters::TCounterPtr& httpInflightSize,
        const ::NMonitoring::TDynamicCounters::TCounterPtr& httpDataRps)
        : TActorCoroImpl(256_KB), ReadActorFactoryCfg(readActorFactoryCfg), InputIndex(inputIndex),
        TxId(txId), RetryStuff(retryStuff), ReadSpec(readSpec), ComputeActorId(computeActorId),
        PathIndex(pathIndex), Path(path), Url(url), MaxBlocksInFly(maxBlocksInFly), ArrowReader(arrowReader),
        DeferredQueueSize(deferredQueueSize), HttpInflightSize(httpInflightSize), HttpDataRps(httpDataRps)
    {}

    ~TS3ReadCoroImpl() override {
        if (DeferredEvents.size() && DeferredQueueSize) {
            DeferredQueueSize->Sub(DeferredEvents.size());
        }
    }

    bool IsDownloadNeeded() const {
        return !ReadSpec->Arrow || !ReadSpec->Compression.empty();
    }

    bool Next(TString& value) {
        if (InputFinished)
            return false;

        if (Paused || DeferredEvents.empty()) {
            auto ev = WaitForSpecificEvent<TEvPrivate::TEvReadStarted
                                         , TEvPrivate::TEvDataPart
                                         , TEvPrivate::TEvReadFinished
                                         , TEvPrivate::TEvPause
                                         , TEvPrivate::TEvContinue
                                         , NActors::TEvents::TEvPoison>();

            switch (const auto etype = ev->GetTypeRewrite()) {
                case TEvPrivate::TEvPause::EventType:
                    Paused = true;
                    break;
                case TEvPrivate::TEvContinue::EventType:
                    Paused = false;
                    break;
                case NActors::TEvents::TEvPoison::EventType:
                    RetryStuff->Cancel();
                    throw TS3ReadAbort();
                default:
                    DeferredEvents.push(std::move(ev));
                    if (DeferredQueueSize) {
                        DeferredQueueSize->Inc();
                    }
                    break;
            }
        }

        if (Paused || DeferredEvents.empty()) {
            value.clear();
            return true;
        }

        THolder<IEventHandle> ev;
        ev.Swap(DeferredEvents.front());
        DeferredEvents.pop();
        if (DeferredQueueSize) {
            DeferredQueueSize->Dec();
        }

        switch (const auto etype = ev->GetTypeRewrite()) {
            case TEvPrivate::TEvReadStarted::EventType:
                ErrorText.clear();
                Issues.Clear();
                value.clear();
                RetryStuff->NextRetryDelay = RetryStuff->GetRetryState()->GetNextRetryDelay(HttpResponseCode = ev->Get<TEvPrivate::TEvReadStarted>()->HttpResponseCode);
                LOG_CORO_D("TS3ReadCoroImpl", "TEvReadStarted, Url: " << RetryStuff->Url << ", Offset: " << RetryStuff->Offset << ", Http code: " << HttpResponseCode << ", retry after: " << RetryStuff->NextRetryDelay << ", request id: [" << RetryStuff->RequestId << "]");
                if (!RetryStuff->NextRetryDelay) { // Success or not retryable
                    RetryStuff->RetryState = nullptr;
                }
                return true;
            case TEvPrivate::TEvReadFinished::EventType:
                Issues = std::move(ev->Get<TEvPrivate::TEvReadFinished>()->Issues);

                if (HttpResponseCode >= 300) {
                    ServerReturnedError = true;
                    Issues.AddIssue(TIssue{TStringBuilder() << "HTTP error code: " << HttpResponseCode});
                }

                if (Issues) {
                    LOG_CORO_D("TS3ReadCoroImpl", "TEvReadFinished. Url: " << RetryStuff->Url << ". Issues: " << Issues.ToOneLineString());
                    if (!RetryStuff->NextRetryDelay) {
                        InputFinished = true;
                        LOG_CORO_W("TS3ReadCoroImpl", "ReadError: " << Issues.ToOneLineString() << ", Url: " << RetryStuff->Url << ", Offset: " << RetryStuff->Offset << ", LastOffset: " << LastOffset << ", LastData: " << GetLastDataAsText() << ", request id: [" << RetryStuff->RequestId << "]");
                        throw TS3ReadError(); // Don't pass control to data parsing, because it may validate eof and show wrong issues about incorrect data format
                    }
                }

                if (!RetryStuff->IsCancelled() && RetryStuff->NextRetryDelay && RetryStuff->SizeLimit > 0ULL) {
                    LOG_CORO_D("TS3ReadCoroImpl", "Retry Download in " << RetryStuff->NextRetryDelay << ", Url: " << RetryStuff->Url << ", Offset: " << RetryStuff->Offset << ", request id: [" << RetryStuff->RequestId << "], Issues: " << Issues.ToOneLineString());
                    GetActorSystem()->Schedule(*RetryStuff->NextRetryDelay, new IEventHandle(ParentActorId, SelfActorId, new TEvPrivate::TEvRetryEventFunc(std::bind(&DownloadStart, RetryStuff, GetActorSystem(), SelfActorId, ParentActorId, PathIndex, HttpInflightSize))));
                    value.clear();
                } else {
                    LOG_CORO_D("TS3ReadCoroImpl", "TEvReadFinished, Url: " << RetryStuff->Url << ", Offset: " << RetryStuff->Offset << ", LastOffset: " << LastOffset << ", Error: " << ServerReturnedError << ", request id: [" << RetryStuff->RequestId << "]");
                    InputFinished = true;
                    if (ServerReturnedError) {
                        throw TS3ReadError(); // Don't pass control to data parsing, because it may validate eof and show wrong issues about incorrect data format
                    }
                    return false; // end of data (real data, not an error)
                }
                return true;
            case TEvPrivate::TEvDataPart::EventType:
                if (HttpDataRps) {
                    HttpDataRps->Inc();
                }
                if (200L == HttpResponseCode || 206L == HttpResponseCode) {
                    value = ev->Get<TEvPrivate::TEvDataPart>()->Result.Extract();
                    IngressBytes += value.size();
                    RetryStuff->Offset += value.size();
                    RetryStuff->SizeLimit -= value.size();
                    LastOffset = RetryStuff->Offset;
                    LastData = value;
                    LOG_CORO_T("TS3ReadCoroImpl", "TEvDataPart, size: " << value.size() << ", Url: " << RetryStuff->Url << ", Offset (updated): " << RetryStuff->Offset << ", request id: [" << RetryStuff->RequestId << "]");
                    Send(ComputeActorId, new IDqComputeActorAsyncInput::TEvNewAsyncInputDataArrived(InputIndex));
                } else if (HttpResponseCode && !RetryStuff->IsCancelled() && !RetryStuff->NextRetryDelay) {
                    ServerReturnedError = true;
                    if (ErrorText.size() < 256_KB)
                        ErrorText.append(ev->Get<TEvPrivate::TEvDataPart>()->Result.Extract());
                    else if (!ErrorText.EndsWith(TruncatedSuffix))
                        ErrorText.append(TruncatedSuffix);
                    value.clear();
                    LOG_CORO_W("TS3ReadCoroImpl", "TEvDataPart, ERROR: " << ErrorText << ", Url: " << RetryStuff->Url << ", Offset: " << RetryStuff->Offset << ", LastOffset: " << LastOffset << ", LastData: " << GetLastDataAsText() << ", request id: [" << RetryStuff->RequestId << "]");
                }
                return true;
            default:
                return false;
        }
    }
private:
    ui64 GetIngressDelta() {
        auto currentIngressBytes = IngressBytes;
        IngressBytes = 0;
        return currentIngressBytes;
    }

    void WaitFinish() {
        LOG_CORO_D("TS3ReadCoroImpl", "WaitFinish: " << Path);
        if (InputFinished)
            return;

        while (true) {
            const auto ev = WaitForSpecificEvent<TEvPrivate::TEvReadStarted
                                               , TEvPrivate::TEvDataPart
                                               , TEvPrivate::TEvReadFinished
                                               , TEvPrivate::TEvPause
                                               , TEvPrivate::TEvContinue
                                               , NActors::TEvents::TEvPoison>();

            const auto etype = ev->GetTypeRewrite();
            switch (etype) {
                case TEvPrivate::TEvReadFinished::EventType:
                    Issues = std::move(ev->Get<TEvPrivate::TEvReadFinished>()->Issues);
                    LOG_CORO_D("TS3ReadCoroImpl", "TEvReadFinished: " << Path << " " << Issues.ToOneLineString());
                    break;
                case NActors::TEvents::TEvPoison::EventType:
                    RetryStuff->Cancel();
                    throw TS3ReadAbort();
                default:
                    continue;
            }
            InputFinished = true;
            return;
        }
    }

    template<typename EvType>
    void WaitEvent() {
        auto event = WaitForEvent();
        TVector<THolder<IEventBase>> otherEvents;
        while (!event->CastAsLocal<EvType>()) {
            if (event->CastAsLocal<NActors::TEvents::TEvPoison>()) {
                throw TS3ReadAbort();
            }
            
            if (!event->CastAsLocal<TEvPrivate::TEvPause>() && !event->CastAsLocal<TEvPrivate::TEvContinue>() && !event->CastAsLocal<TEvPrivate::TEvReadFinished>()) {
                otherEvents.push_back(event->ReleaseBase());
            }
            
            event = WaitForEvent();
        }

        for (auto& e: otherEvents) {
            Send(SelfActorId, e.Release());
        }
    }

    void Run() final try {

        LOG_CORO_D("TS3ReadCoroImpl", "Run" << ", Path: " << Path);

        NYql::NDqProto::StatusIds::StatusCode fatalCode = NYql::NDqProto::StatusIds::EXTERNAL_ERROR;

        TIssue exceptIssue;
        bool isLocal = Url.StartsWith("file://");
        bool needWaitFinish = !isLocal;
        try {
            if (ReadSpec->Arrow) {
                TArrowFileDesc fileDesc(Url + Path, RetryStuff->Gateway, RetryStuff->Headers, RetryStuff->RetryPolicy, RetryStuff->SizeLimit, ReadSpec->Format);
                if (IsDownloadNeeded()) {
                    // Read file entirely
                    std::unique_ptr<NDB::ReadBuffer> buffer;
                    if (isLocal) {
                        buffer = std::make_unique<NDB::ReadBufferFromFile>(Url.substr(7) + Path);
                    } else {
                        buffer = std::make_unique<TReadBufferFromStream>(this);
                    }

                    const auto decompress(MakeDecompressor(*buffer, ReadSpec->Compression));
                    YQL_ENSURE(ReadSpec->Compression.empty() == !decompress, "Unsupported " << ReadSpec->Compression << " compression.");
                    auto& readBuffer = decompress ? *decompress : *buffer;
                    TStringBuilder sb;
                    TBuffer buff(256_KB);

                    while (!readBuffer.eof()) {
                        if (!readBuffer.hasPendingData()) {
                            if (!readBuffer.next()) {
                                break;
                            }
                        }
                        auto bytesReaded = readBuffer.read(buff.data(), 256_KB);
                        sb.append(buff.data(), buff.data() + bytesReaded);
                    }

                    fileDesc.Contents = sb;

                } else {
                    needWaitFinish = false;
                }
                auto actorSystem = GetActorSystem();
                auto onResolve = [actorSystem, actorId = this->SelfActorId] {
                                            actorSystem->Send(new IEventHandle(actorId, actorId, new TEvPrivate::TEvFutureResolved()));
                                        };
                auto future = ArrowReader->GetSchema(fileDesc);
                future.Subscribe([onResolve](const NThreading::TFuture<IArrowReader::TSchemaResponse>&) {
                                                onResolve();
                                            });
                WaitEvent<TEvPrivate::TEvFutureResolved>();
                auto result = future.GetValue();
                std::shared_ptr<arrow::Schema> schema = result.Schema;
                std::vector<int> columnIndices;
                std::vector<TColumnConverter> columnConverters;
                for (int i = 0; i < ReadSpec->ArrowSchema->num_fields(); ++i) {
                    const auto& targetField = ReadSpec->ArrowSchema->field(i);
                    auto srcFieldIndex = schema->GetFieldIndex(targetField->name());
                    YQL_ENSURE(srcFieldIndex != -1, "Missing field: " << targetField->name());
                    auto targetType = targetField->type();
                    auto originalType = schema->field(srcFieldIndex)->type();
                    YQL_ENSURE(!originalType->layout().has_dictionary, "Unsupported dictionary encoding is used for field: "
                               << targetField->name() << ", type: " << originalType->ToString());
                    if (targetType->Equals(originalType)) {
                        columnConverters.emplace_back();
                    } else {
                        YQL_ENSURE(arrow::compute::CanCast(*originalType, *targetType), "Mismatch type for field: " << targetField->name() << ", expected: "
                            << targetType->ToString() << ", got: " << originalType->ToString());
                        columnConverters.emplace_back([targetType](const std::shared_ptr<arrow::Array>& value) {
                            auto res = arrow::compute::Cast(*value, targetType);
                            THROW_ARROW_NOT_OK(res.status());
                            return std::move(res).ValueOrDie();
                        });
                    }

                    columnIndices.push_back(srcFieldIndex);
                }

                fileDesc.Cookie = result.Cookie;
                TArrowParquetBatchReader reader(std::move(fileDesc),
                                                ArrowReader,
                                                result.NumRowGroups,
                                                std::move(columnIndices),
                                                std::move(columnConverters),
                                                onResolve,
                                                [&] { WaitEvent<TEvPrivate::TEvFutureResolved>(); });
                ProcessBatches<std::shared_ptr<arrow::RecordBatch>, TEvPrivate::TEvNextRecordBatch>(reader, isLocal);
            } else {
                std::unique_ptr<NDB::ReadBuffer> buffer;
                if (isLocal) {
                    buffer = std::make_unique<NDB::ReadBufferFromFile>(Url.substr(7) + Path);
                } else {
                    buffer = std::make_unique<TReadBufferFromStream>(this);
                }

                const auto decompress(MakeDecompressor(*buffer, ReadSpec->Compression));
                YQL_ENSURE(ReadSpec->Compression.empty() == !decompress, "Unsupported " << ReadSpec->Compression << " compression.");

                auto stream = std::make_unique<NDB::InputStreamFromInputFormat>(NDB::FormatFactory::instance().getInputFormat(ReadSpec->Format, decompress ? *decompress : *buffer, NDB::Block(ReadSpec->CHColumns), nullptr, ReadActorFactoryCfg.RowsInBatch, ReadSpec->Settings));
                TBlockReader reader(std::move(stream));
                ProcessBatches<NDB::Block, TEvPrivate::TEvNextBlock>(reader, isLocal);
            }
        } catch (const TS3ReadError&) {
            // Finish reading. Add error from server to issues
            LOG_CORO_D("TS3ReadCoroImpl", "S3 read error. Path: " << Path);
        } catch (const TDtorException&) {
            throw;
        } catch (const NDB::Exception& err) {
            TStringBuilder msgBuilder;
            msgBuilder << err.message();
            if (err.code()) {
                msgBuilder << " (code: " << err.code() << ")";
            }
            exceptIssue.SetMessage(msgBuilder);
            fatalCode = NYql::NDqProto::StatusIds::BAD_REQUEST;
            RetryStuff->Cancel();
        } catch (const std::exception& err) {
            exceptIssue.SetMessage(err.what());
            fatalCode = NYql::NDqProto::StatusIds::INTERNAL_ERROR;
            RetryStuff->Cancel();
        }

        if (needWaitFinish) {
            WaitFinish();
        }

        if (!ErrorText.empty()) {
            TString errorCode;
            TString message;
            if (!ParseS3ErrorResponse(ErrorText, errorCode, message)) {
                message = ErrorText;
            }
            Issues.AddIssues(BuildIssues(HttpResponseCode, errorCode, message));
        }

        if (exceptIssue.GetMessage()) {
            Issues.AddIssue(exceptIssue);
        }

        auto issues = NS3Util::AddParentIssue(TStringBuilder{} << "Error while reading file " << Path, std::move(Issues));
        if (issues)
            Send(ComputeActorId, new IDqComputeActorAsyncInput::TEvAsyncInputError(InputIndex, std::move(issues), fatalCode));
        else
            Send(ParentActorId, new TEvPrivate::TEvFileFinished(PathIndex, GetIngressDelta()));
    } catch (const TS3ReadAbort&) {
        LOG_CORO_D("TS3ReadCoroImpl", "S3 read abort. Path: " << Path);
    } catch (const TDtorException&) {
        return RetryStuff->Cancel();
    } catch (const std::exception& err) {
        Send(ComputeActorId, new IDqComputeActorAsyncInput::TEvAsyncInputError(InputIndex, TIssues{TIssue(TStringBuilder() << "Error while reading file " << Path << ", details: " << err.what())}, NYql::NDqProto::StatusIds::INTERNAL_ERROR));
        return;
    }

    template <typename T, typename TEv>
    void ProcessBatches(IBatchReader<T>& reader, bool isLocal) {
        auto actorSystem = GetActorSystem();
        auto selfActorId = SelfActorId;
        size_t cntBlocksInFly = 0;
        if (isLocal) {
            for (;;) {
                T batch;

                if (!reader.Next(batch)) {
                    break;
                }
                if (++cntBlocksInFly > MaxBlocksInFly) {
                    WaitEvent<TEvPrivate::TEvBlockProcessed>();
                    --cntBlocksInFly;
                }
                Send(ParentActorId, new TEv(batch, PathIndex, [actorSystem, selfActorId]() {
                    actorSystem->Send(new IEventHandle(selfActorId, selfActorId, new TEvPrivate::TEvBlockProcessed()));
                }, GetIngressDelta()));
            }
            while (cntBlocksInFly--) {
                WaitEvent<TEvPrivate::TEvBlockProcessed>();
            }
        } else {
            for (;;) {
                T batch;
                if (!reader.Next(batch)) {
                    break;
                }
                Send(ParentActorId, new TEv(batch, PathIndex, [](){}, GetIngressDelta()));
            }
        }
    }

    void ProcessUnexpectedEvent(TAutoPtr<IEventHandle> ev) final {
        TStringBuilder message;
        message << "Error while reading file " << Path << ", details: "
                << "S3 read. Unexpected message type " << Hex(ev->GetTypeRewrite());
        if (auto* eventBase = ev->GetBase()) {
            message << " (" << eventBase->ToStringHeader() << ")";
        }
        Send(ComputeActorId, new IDqComputeActorAsyncInput::TEvAsyncInputError(InputIndex, TIssues{TIssue(message)}, NYql::NDqProto::StatusIds::INTERNAL_ERROR));
    }

    TString GetLastDataAsText() {

        if (LastData.empty()) {
            return "[]";
        }

        auto begin = const_cast<char*>(LastData.data());
        auto end = begin + LastData.size();

        TStringBuilder result;

        result << "[";

        if (LastData.size() > 32) {
            begin += LastData.size() - 32;
            result << "...";
        }

        while (begin < end) {
            char c = *begin++;
            if (c >= 32 && c <= 126) {
                result << c;
            } else {
                result << "\\" << Hex(static_cast<ui8>(c));
            }
        }

        result << "]";

        return result;
    }

private:
    const TS3ReadActorFactoryConfig ReadActorFactoryCfg;
    const ui64 InputIndex;
    const TTxId TxId;
    const TRetryStuff::TPtr RetryStuff;
    const TReadSpec::TPtr ReadSpec;
    const TString Format, RowType, Compression;
    const NActors::TActorId ComputeActorId;
    const size_t PathIndex;
    const TString Path;
    const TString Url;

    bool InputFinished = false;
    long HttpResponseCode = 0L;
    bool ServerReturnedError = false;
    TString ErrorText;
    TIssues Issues;

    std::size_t LastOffset = 0;
    TString LastData;
    std::size_t MaxBlocksInFly = 2;
    IArrowReader::TPtr ArrowReader;
    ui64 IngressBytes = 0;
    bool Paused = false;
    std::queue<THolder<IEventHandle>> DeferredEvents;
    const ::NMonitoring::TDynamicCounters::TCounterPtr DeferredQueueSize;
    const ::NMonitoring::TDynamicCounters::TCounterPtr HttpInflightSize;
    const ::NMonitoring::TDynamicCounters::TCounterPtr HttpDataRps;
};

class TS3ReadCoroActor : public TActorCoro {
public:
    TS3ReadCoroActor(THolder<TS3ReadCoroImpl> impl, TRetryStuff::TPtr retryStuff, size_t pathIndex, bool isDownloadNeeded, const ::NMonitoring::TDynamicCounters::TCounterPtr& httpInflightSize)
        : TActorCoro(THolder<TActorCoroImpl>(impl.Release()))
        , RetryStuff(std::move(retryStuff))
        , PathIndex(pathIndex)
        , IsDownloadNeeded(isDownloadNeeded)
        , HttpInflightSize(httpInflightSize)
    {}
private:
    void Registered(TActorSystem* actorSystem, const TActorId& parent) override {
        TActorCoro::Registered(actorSystem, parent); // Calls TActorCoro::OnRegister and sends bootstrap event to ourself.
        if (IsDownloadNeeded && RetryStuff->Url.substr(0, 6) != "file://") {
            LOG_DEBUG_S(*actorSystem, NKikimrServices::KQP_COMPUTE, "TS3ReadCoroActor" << ": " << SelfId() << ", TxId: " << RetryStuff->TxId << ". " << "Start Download, Url: " << RetryStuff->Url << ", Offset: " << RetryStuff->Offset << ", request id: [" << RetryStuff->RequestId << "]");
            DownloadStart(RetryStuff, actorSystem, SelfId(), parent, PathIndex, HttpInflightSize);
        }
    }

    const TRetryStuff::TPtr RetryStuff;
    const size_t PathIndex;
    const bool IsDownloadNeeded;
    const ::NMonitoring::TDynamicCounters::TCounterPtr HttpInflightSize;
};

class TS3StreamReadActor : public TActorBootstrapped<TS3StreamReadActor>, public IDqComputeActorAsyncInput {
public:
    TS3StreamReadActor(
        ui64 inputIndex,
        const TTxId& txId,
        IHTTPGateway::TPtr gateway,
        const THolderFactory& holderFactory,
        const TString& url,
        const TString& token,
        const TString& pattern,
        ES3PatternVariant patternVariant,
        TPathList&& paths,
        bool addPathIndex,
        ui64 startPathIndex,
        const TReadSpec::TPtr& readSpec,
        const NActors::TActorId& computeActorId,
        const IRetryPolicy<long>::TPtr& retryPolicy,
        const std::size_t maxBlocksInFly,
        IArrowReader::TPtr arrowReader,
        const TS3ReadActorFactoryConfig& readActorFactoryCfg,
        ::NMonitoring::TDynamicCounterPtr counters,
        ::NMonitoring::TDynamicCounterPtr taskCounters,
        ui64 fileSizeLimit
    )   : ReadActorFactoryCfg(readActorFactoryCfg)
        , Gateway(std::move(gateway))
        , HolderFactory(holderFactory)
        , InputIndex(inputIndex)
        , TxId(txId)
        , ComputeActorId(computeActorId)
        , RetryPolicy(retryPolicy)
        , Url(url)
        , Token(token)
        , Pattern(pattern)
        , PatternVariant(patternVariant)
        , Paths(std::move(paths))
        , AddPathIndex(addPathIndex)
        , StartPathIndex(startPathIndex)
        , ReadSpec(readSpec)
        , MaxBlocksInFly(maxBlocksInFly)
        , ArrowReader(std::move(arrowReader))
        , Counters(std::move(counters))
        , TaskCounters(std::move(taskCounters))
        , FileSizeLimit(fileSizeLimit) {
        if (Counters) {
            QueueDataSize = Counters->GetCounter("QueueDataSize");
            QueueDataLimit = Counters->GetCounter("QueueDataLimit");
            QueueBlockCount = Counters->GetCounter("QueueBlockCount");
            DownloadPaused = Counters->GetCounter("DownloadPaused");
            QueueDataLimit->Add(ReadActorFactoryCfg.DataInflight);
        }
        if (TaskCounters) {
            TaskQueueDataSize = TaskCounters->GetCounter("QueueDataSize");
            TaskQueueDataLimit = TaskCounters->GetCounter("QueueDataLimit");
            TaskDownloadPaused = TaskCounters->GetCounter("DownloadPaused");
            DeferredQueueSize = TaskCounters->GetCounter("DeferredQueueSize");
            HttpInflightSize = TaskCounters->GetCounter("HttpInflightSize");
            HttpInflightLimit = TaskCounters->GetCounter("HttpInflightLimit");
            HttpDataRps = TaskCounters->GetCounter("HttpDataRps", true);
            TaskQueueDataLimit->Add(ReadActorFactoryCfg.DataInflight);
        }
    }

    void Bootstrap() {
        LOG_D("TS3StreamReadActor", "Bootstrap");
        FileQueueActor = RegisterWithSameMailbox(new TS3FileQueueActor{
            TxId,
            std::move(Paths),
            ReadActorFactoryCfg.MaxInflight * 2,
            FileSizeLimit,
            Gateway,
            Url,
            Token,
            Pattern,
            PatternVariant,
            ES3PatternType::Wildcard});
        SendPathRequest();
        Become(&TS3StreamReadActor::StateFunc);
    }

    bool TryRegisterCoro() {
        if (ObjectPathCache.empty()) {
            // no path is pending
            return false;
        }

        if (QueueTotalDataSize > ReadActorFactoryCfg.DataInflight) {
            // too large data inflight
            return false;
        }
        if (DownloadInflight >= ReadActorFactoryCfg.MaxInflight) {
            // too large download inflight
            return false;
        }
        RegisterCoro();
        return true;
    }

    void RegisterCoro() {
        DownloadInflight++;
        const auto& objectPath = ReadPathFromCache();
        const TString requestId = CreateGuidAsString();
        auto stuff = std::make_shared<TRetryStuff>(
            Gateway,
            Url + objectPath.Path,
            MakeHeaders(Token, requestId),
            objectPath.Size,
            TxId,
            requestId,
            RetryPolicy);
        auto pathIndex = objectPath.PathIndex + StartPathIndex;
        RetryStuffForFile.emplace(pathIndex, stuff);
        if (TaskCounters) {
            HttpInflightLimit->Add(Gateway->GetBuffersSizePerStream());
        }
        LOG_D(
            "TS3StreamReadActor",
            "RegisterCoro with path " << objectPath.Path << " with pathIndex "
                                      << pathIndex);
        ::NMonitoring::TDynamicCounters::TCounterPtr inflightCounter;
        auto impl = MakeHolder<TS3ReadCoroImpl>(
            InputIndex,
            TxId,
            ComputeActorId,
            stuff,
            ReadSpec,
            pathIndex,
            objectPath.Path,
            Url,
            MaxBlocksInFly,
            ArrowReader,
            ReadActorFactoryCfg,
            DeferredQueueSize,
            HttpInflightSize,
            HttpDataRps);
        auto isDownloadNeeded = impl->IsDownloadNeeded();
        const auto& httpInflightSize = impl->HttpInflightSize;
        CoroActors.insert(RegisterWithSameMailbox(new TS3ReadCoroActor(
            std::move(impl), std::move(stuff), pathIndex, isDownloadNeeded, httpInflightSize)));
    }

    TObjectPath ReadPathFromCache() {
        Y_ENSURE(!ObjectPathCache.empty());
        auto object = ObjectPathCache.back();
        ObjectPathCache.pop_back();
        if (ObjectPathCache.empty() && !IsObjectQueueEmpty) {
            SendPathRequest();
        }
        return object;
    }
    void SendPathRequest() {
        Y_ENSURE(!IsWaitingObjectQueueResponse);
        LOG_D("TS3StreamReadActor", "SendPathRequest " << ReadActorFactoryCfg.MaxInflight);
        Send(
            FileQueueActor,
            std::make_unique<TS3FileQueueActor::TEvPrivatePrivate::TEvGetNextFile>(
                ReadActorFactoryCfg.MaxInflight));
        IsWaitingObjectQueueResponse = true;
    }

    static constexpr char ActorName[] = "S3_STREAM_READ_ACTOR";

private:
    class TBoxedBlock : public TComputationValue<TBoxedBlock> {
    public:
        TBoxedBlock(TMemoryUsageInfo* memInfo, NDB::Block& block)
            : TComputationValue(memInfo)
        {
            Block.swap(block);
        }
    private:
        NUdf::TStringRef GetResourceTag() const final {
            return NUdf::TStringRef::Of("ClickHouseClient.Block");
        }

        void* GetResource() final {
            return &Block;
        }

        NDB::Block Block;
    };

    class TReadyBlock {
    public:
        TReadyBlock(TEvPrivate::TEvNextBlock::TPtr& event) : PathInd(event->Get()->PathIndex), Functor (std::move(event->Get()->Functor)) { Block.swap(event->Get()->Block); }
        TReadyBlock(TEvPrivate::TEvNextRecordBatch::TPtr& event) : Batch(event->Get()->Batch), PathInd(event->Get()->PathIndex), Functor(std::move(event->Get()->Functor)) {}
        NDB::Block Block;
        std::shared_ptr<arrow::RecordBatch> Batch;
        size_t PathInd;
        std::function<void()> Functor;
    };

    void SaveState(const NDqProto::TCheckpoint&, NDqProto::TSourceState&) final {}
    void LoadState(const NDqProto::TSourceState&) final {}
    void CommitState(const NDqProto::TCheckpoint&) final {}
    ui64 GetInputIndex() const final { return InputIndex; }

    ui64 GetIngressBytes() override {
        return IngressBytes;
    }

    ui64 GetBlockSize(const TReadyBlock& block) const {
        return ReadSpec->Arrow ? NUdf::GetSizeOfArrowBatchInBytes(*block.Batch) : block.Block.bytes();
    }

    void ReportMemoryUsage() const {
        const TInstant now = TInstant::Now();
        if (now - LastMemoryReport < MEMORY_USAGE_REPORT_PERIOD) {
            return;
        }
        LastMemoryReport = now;
        size_t blocksTotalSize = 0;
        for (const auto& block : Blocks) {
            blocksTotalSize += GetBlockSize(block);
        }
        LOG_D("TS3StreamReadActor", "Memory usage. Ready blocks: " << Blocks.size() << ". Ready blocks total size: " << blocksTotalSize);
    }

    i64 GetAsyncInputData(TUnboxedValueVector& output, TMaybe<TInstant>&, bool& finished, i64 free) final {
        ReportMemoryUsage();

        i64 total = 0LL;
        if (!Blocks.empty()) do {
            const i64 s = GetBlockSize(Blocks.front());

            NUdf::TUnboxedValue value;
            if (ReadSpec->Arrow) {
                const auto& batch = *Blocks.front().Batch;

                NUdf::TUnboxedValue* structItems = nullptr;
                auto structObj = ArrowRowContainerCache.NewArray(HolderFactory, 1 + batch.num_columns(), structItems);
                for (int i = 0; i < batch.num_columns(); ++i) {
                    structItems[ReadSpec->ColumnReorder[i]] = HolderFactory.CreateArrowBlock(arrow::Datum(batch.column_data(i)));
                }

                structItems[ReadSpec->BlockLengthPosition] = HolderFactory.CreateArrowBlock(arrow::Datum(std::make_shared<arrow::UInt64Scalar>(batch.num_rows())));
                value = structObj;
            } else {
                value = HolderFactory.Create<TBoxedBlock>(Blocks.front().Block);
            }

            Blocks.front().Functor();

            if (AddPathIndex) {
                NUdf::TUnboxedValue* tupleItems = nullptr;
                auto tuple = ContainerCache.NewArray(HolderFactory, 2, tupleItems);
                *tupleItems++ = value;
                *tupleItems++ = NUdf::TUnboxedValuePod(Blocks.front().PathInd);
                value = tuple;
            }

            free -= s;
            total += s;
            output.emplace_back(std::move(value));
            Blocks.pop_front();
            QueueTotalDataSize -= s;
            if (Counters) {
                QueueDataSize->Sub(s);
                QueueBlockCount->Dec();
            }
            if (TaskCounters) {
                TaskQueueDataSize->Sub(s);
            }
            TryRegisterCoro();
        } while (!Blocks.empty() && free > 0LL && GetBlockSize(Blocks.front()) <= size_t(free));

        MaybeContinue();

        finished = LastFileWasProcessed();
        if (finished) {
            ContainerCache.Clear();
            ArrowTupleContainerCache.Clear();
            ArrowRowContainerCache.Clear();
        }
        return total;
    }

    // IActor & IDqComputeActorAsyncInput
    void PassAway() override { // Is called from Compute Actor
        LOG_D("TS3StreamReadActor", "PassAway");
        if (Counters) {
            QueueDataSize->Sub(QueueTotalDataSize);
            QueueBlockCount->Sub(Blocks.size());
            QueueDataLimit->Sub(ReadActorFactoryCfg.DataInflight);
        }
        if (TaskCounters) {
            TaskQueueDataSize->Sub(QueueTotalDataSize);
            TaskQueueDataLimit->Sub(ReadActorFactoryCfg.DataInflight);
            HttpInflightLimit->Sub(Gateway->GetBuffersSizePerStream() * CoroActors.size());
        }
        if (Paused) {
            if (Counters) {
                DownloadPaused->Dec();
            }
            if (TaskCounters) {
                TaskDownloadPaused->Dec();
            }
        }
        QueueTotalDataSize = 0;

        for (const auto actorId : CoroActors) {
            Send(actorId, new NActors::TEvents::TEvPoison());
        }
        Send(FileQueueActor, new NActors::TEvents::TEvPoison());

        ContainerCache.Clear();
        ArrowTupleContainerCache.Clear();
        ArrowRowContainerCache.Clear();

        TActorBootstrapped<TS3StreamReadActor>::PassAway();
    }

    static IHTTPGateway::THeaders MakeHeaders(const TString& token, const TString& requestId) {
        IHTTPGateway::THeaders headers{TString{"X-Request-ID:"} += requestId};
        if (token) {
            headers.emplace_back(TString("X-YaCloud-SubjectToken:") += token);
        }
        return headers;
    }

    void MaybePause() {
        if (!Paused && QueueTotalDataSize >= ReadActorFactoryCfg.DataInflight) {
            for (const auto actorId : CoroActors) {
                Send(actorId, new TEvPrivate::TEvPause());
            }
            Paused = true;
            if (Counters) {
                DownloadPaused->Inc();
            }
            if (TaskCounters) {
                TaskDownloadPaused->Inc();
            }
        }
    }

    void MaybeContinue() {
        // resume download on 3/4 == 75% to avoid oscillation (hysteresis)
        if (Paused && QueueTotalDataSize * 4 < ReadActorFactoryCfg.DataInflight * 3) {
            for (const auto actorId : CoroActors) {
                Send(actorId, new TEvPrivate::TEvContinue());
            }
            Paused = false;
            if (Counters) {
                DownloadPaused->Dec();
            }
            if (TaskCounters) {
                TaskDownloadPaused->Dec();
            }
        }
    }

    STRICT_STFUNC(StateFunc,
        hFunc(TEvPrivate::TEvRetryEventFunc, HandleRetry);
        hFunc(TEvPrivate::TEvNextBlock, HandleNextBlock);
        hFunc(TEvPrivate::TEvNextRecordBatch, HandleNextRecordBatch);
        hFunc(TEvPrivate::TEvFileFinished, HandleFileFinished);
        hFunc(TEvPrivate::TEvObjectPathBatch, HandleObjectPathBatch);
        hFunc(TEvPrivate::TEvObjectPathReadError, HandleObjectPathReadError);
    )

    void HandleObjectPathBatch(TEvPrivate::TEvObjectPathBatch::TPtr& objectPathBatch) {
        Y_ENSURE(IsWaitingObjectQueueResponse);
        IsWaitingObjectQueueResponse = false;
        ListedFiles += objectPathBatch->Get()->ObjectPaths.size();
        IsObjectQueueEmpty = objectPathBatch->Get()->NoMoreFiles;

        ObjectPathCache.insert(
            ObjectPathCache.end(),
            std::make_move_iterator(objectPathBatch->Get()->ObjectPaths.begin()),
            std::make_move_iterator(objectPathBatch->Get()->ObjectPaths.end()));
        LOG_W(
            "TS3StreamReadActor",
            "HandleObjectPathBatch " << ObjectPathCache.size() << " IsObjectQueueEmpty "
                                     << IsObjectQueueEmpty << " MaxInflight " << ReadActorFactoryCfg.MaxInflight);
        while (TryRegisterCoro()) {}
    }
    void HandleObjectPathReadError(TEvPrivate::TEvObjectPathReadError::TPtr& result) {
        IsObjectQueueEmpty = true;
        LOG_W("TS3StreamReadActor", "Error while object listing, details: TEvObjectPathReadError: " << result->Get()->Issues.ToOneLineString());
        auto issues = NS3Util::AddParentIssue(TStringBuilder{} << "Error while object listing", TIssues{result->Get()->Issues});
        Send(ComputeActorId, new TEvAsyncInputError(InputIndex, std::move(issues), NYql::NDqProto::StatusIds::EXTERNAL_ERROR));
    }

    void HandleRetry(TEvPrivate::TEvRetryEventFunc::TPtr& retry) {
        return retry->Get()->Functor();
    }

    void HandleNextBlock(TEvPrivate::TEvNextBlock::TPtr& next) {
        YQL_ENSURE(!ReadSpec->Arrow);
        IngressBytes += next->Get()->IngressDelta;
        auto size = next->Get()->Block.bytes();
        QueueTotalDataSize += size;
        if (Counters) {
            QueueBlockCount->Inc();
            QueueDataSize->Add(size);
        }
        if (TaskCounters) {
            TaskQueueDataSize->Add(size);
        }
        MaybePause();
        Blocks.emplace_back(next);
        Send(ComputeActorId, new IDqComputeActorAsyncInput::TEvNewAsyncInputDataArrived(InputIndex));
        ReportMemoryUsage();
    }

    void HandleNextRecordBatch(TEvPrivate::TEvNextRecordBatch::TPtr& next) {
        YQL_ENSURE(ReadSpec->Arrow);
        IngressBytes += next->Get()->IngressDelta;
        auto size = NUdf::GetSizeOfArrowBatchInBytes(*next->Get()->Batch);
        QueueTotalDataSize += size;
        if (Counters) {
            QueueBlockCount->Inc();
            QueueDataSize->Add(size);
        }
        if (TaskCounters) {
            TaskQueueDataSize->Add(size);
        }
        MaybePause();
        Blocks.emplace_back(next);
        Send(ComputeActorId, new IDqComputeActorAsyncInput::TEvNewAsyncInputDataArrived(InputIndex));
        ReportMemoryUsage();
    }

    void HandleFileFinished(TEvPrivate::TEvFileFinished::TPtr& ev) {
        CoroActors.erase(ev->Sender);
        IngressBytes += ev->Get()->IngressDelta;
        RetryStuffForFile.erase(ev->Get()->PathIndex);

        if (TaskCounters) {
            HttpInflightLimit->Sub(Gateway->GetBuffersSizePerStream());
        }
        DownloadInflight--;
        CompletedFiles++;
        if (!ObjectPathCache.empty()) {
            TryRegisterCoro();
        } else {
            /*
            If an empty range is being downloaded on the last file,
            then we need to pass the information to Compute Actor that
            the download of all data is finished in this place
            */
            if (LastFileWasProcessed()) {
                Send(ComputeActorId, new IDqComputeActorAsyncInput::TEvNewAsyncInputDataArrived(InputIndex));
            }
        }
    }

    bool LastFileWasProcessed() const {
        return Blocks.empty() && (ListedFiles == CompletedFiles) && IsObjectQueueEmpty;
    }

    const TS3ReadActorFactoryConfig ReadActorFactoryCfg;
    const IHTTPGateway::TPtr Gateway;
    THashMap<size_t, TRetryStuff::TPtr> RetryStuffForFile;
    const THolderFactory& HolderFactory;
    TPlainContainerCache ContainerCache;
    TPlainContainerCache ArrowTupleContainerCache;
    TPlainContainerCache ArrowRowContainerCache;

    const ui64 InputIndex;
    const TTxId TxId;
    const NActors::TActorId ComputeActorId;
    const IRetryPolicy<long>::TPtr RetryPolicy;

    const TString Url;
    const TString Token;
    const TString Pattern;
    const ES3PatternVariant PatternVariant;
    TPathList Paths;
    std::vector<TObjectPath> ObjectPathCache;
    bool IsObjectQueueEmpty = false;
    bool IsWaitingObjectQueueResponse = false;
    const bool AddPathIndex;
    const ui64 StartPathIndex;
    size_t ListedFiles = 0;
    size_t CompletedFiles = 0;
    const TReadSpec::TPtr ReadSpec;
    std::deque<TReadyBlock> Blocks;
    const std::size_t MaxBlocksInFly;
    IArrowReader::TPtr ArrowReader;
    ui64 IngressBytes = 0;
    mutable TInstant LastMemoryReport = TInstant::Now();
    ui64 QueueTotalDataSize = 0;
    ::NMonitoring::TDynamicCounters::TCounterPtr QueueDataSize;
    ::NMonitoring::TDynamicCounters::TCounterPtr QueueDataLimit;
    ::NMonitoring::TDynamicCounters::TCounterPtr QueueBlockCount;
    ::NMonitoring::TDynamicCounters::TCounterPtr DownloadPaused;
    ::NMonitoring::TDynamicCounters::TCounterPtr DeferredQueueSize;
    ::NMonitoring::TDynamicCounters::TCounterPtr TaskDownloadPaused;
    ::NMonitoring::TDynamicCounters::TCounterPtr TaskQueueDataSize;
    ::NMonitoring::TDynamicCounters::TCounterPtr TaskQueueDataLimit;
    ::NMonitoring::TDynamicCounters::TCounterPtr HttpInflightSize;
    ::NMonitoring::TDynamicCounters::TCounterPtr HttpInflightLimit;
    ::NMonitoring::TDynamicCounters::TCounterPtr HttpDataRps;
    ::NMonitoring::TDynamicCounterPtr Counters;
    ::NMonitoring::TDynamicCounterPtr TaskCounters;
    ui64 DownloadInflight = 0;
    std::set<NActors::TActorId> CoroActors;
    NActors::TActorId FileQueueActor;
    bool Paused = false;
    const ui64 FileSizeLimit;
};

using namespace NKikimr::NMiniKQL;
// the same func exists in clickhouse client udf :(
NDB::DataTypePtr PgMetaToClickHouse(const TPgType* type) {
    auto typeId = type->GetTypeId();
    TTypeInfoHelper typeInfoHelper;
    auto* pgDescription = typeInfoHelper.FindPgTypeDescription(typeId);
    Y_ENSURE(pgDescription);
    const auto typeName = pgDescription->Name;
    using NUdf::TStringRef;
    if (typeName == TStringRef("bool")) {
        return std::make_shared<NDB::DataTypeUInt8>();
    }

    if (typeName == TStringRef("int4")) {
        return std::make_shared<NDB::DataTypeInt32>();
    }

    if (typeName == TStringRef("int8")) {
        return std::make_shared<NDB::DataTypeInt64>();
    }

    if (typeName == TStringRef("float4")) {
        return std::make_shared<NDB::DataTypeFloat32>();
    }

    if (typeName == TStringRef("float8")) {
        return std::make_shared<NDB::DataTypeFloat64>();
    }
    return std::make_shared<NDB::DataTypeString>();
}

NDB::DataTypePtr PgMetaToNullableClickHouse(const TPgType* type) {
    return makeNullable(PgMetaToClickHouse(type));
}

NDB::DataTypePtr MetaToClickHouse(const TType* type, NSerialization::TSerializationInterval::EUnit unit) {
    switch (type->GetKind()) {
        case TType::EKind::EmptyList:
            return std::make_shared<NDB::DataTypeArray>(std::make_shared<NDB::DataTypeNothing>());
        case TType::EKind::Optional:
            return makeNullable(MetaToClickHouse(static_cast<const TOptionalType*>(type)->GetItemType(), unit));
        case TType::EKind::List:
            return std::make_shared<NDB::DataTypeArray>(MetaToClickHouse(static_cast<const TListType*>(type)->GetItemType(), unit));
        case TType::EKind::Tuple: {
            const auto tupleType = static_cast<const TTupleType*>(type);
            NDB::DataTypes elems;
            elems.reserve(tupleType->GetElementsCount());
            for (auto i = 0U; i < tupleType->GetElementsCount(); ++i)
                elems.emplace_back(MetaToClickHouse(tupleType->GetElementType(i), unit));
            return std::make_shared<NDB::DataTypeTuple>(elems);
        }
        case TType::EKind::Pg:
            return PgMetaToNullableClickHouse(AS_TYPE(TPgType, type));
        case TType::EKind::Data: {
            const auto dataType = static_cast<const TDataType*>(type);
            switch (const auto slot = *dataType->GetDataSlot()) {
            case NUdf::EDataSlot::Int8:
                return std::make_shared<NDB::DataTypeInt8>();
            case NUdf::EDataSlot::Bool:
            case NUdf::EDataSlot::Uint8:
                return std::make_shared<NDB::DataTypeUInt8>();
            case NUdf::EDataSlot::Int16:
                return std::make_shared<NDB::DataTypeInt16>();
            case NUdf::EDataSlot::Uint16:
                return std::make_shared<NDB::DataTypeUInt16>();
            case NUdf::EDataSlot::Int32:
                return std::make_shared<NDB::DataTypeInt32>();
            case NUdf::EDataSlot::Uint32:
                return std::make_shared<NDB::DataTypeUInt32>();
            case NUdf::EDataSlot::Int64:
                return std::make_shared<NDB::DataTypeInt64>();
            case NUdf::EDataSlot::Uint64:
                return std::make_shared<NDB::DataTypeUInt64>();
            case NUdf::EDataSlot::Float:
                return std::make_shared<NDB::DataTypeFloat32>();
            case NUdf::EDataSlot::Double:
                return std::make_shared<NDB::DataTypeFloat64>();
            case NUdf::EDataSlot::String:
            case NUdf::EDataSlot::Utf8:
            case NUdf::EDataSlot::Json:
                return std::make_shared<NDB::DataTypeString>();
            case NUdf::EDataSlot::Date:
            case NUdf::EDataSlot::TzDate:
                return std::make_shared<NDB::DataTypeDate>();
            case NUdf::EDataSlot::Datetime:
            case NUdf::EDataSlot::TzDatetime:
                return std::make_shared<NDB::DataTypeDateTime>("UTC");
            case NUdf::EDataSlot::Timestamp:
            case NUdf::EDataSlot::TzTimestamp:
                return std::make_shared<NDB::DataTypeDateTime64>(6, "UTC");
            case NUdf::EDataSlot::Uuid:
                return std::make_shared<NDB::DataTypeUUID>();
            case NUdf::EDataSlot::Interval:
                return NSerialization::GetInterval(unit);
            default:
                throw yexception() << "Unsupported data slot in MetaToClickHouse: " << slot;
            }
        }
        default:
            throw yexception() << "Unsupported type kind in MetaToClickHouse: " << type->GetKindAsStr();
    }
    return nullptr;
}

NDB::FormatSettings::DateTimeFormat ToDateTimeFormat(const TString& formatName) {
    static TMap<TString, NDB::FormatSettings::DateTimeFormat> formats{
        {"POSIX", NDB::FormatSettings::DateTimeFormat::POSIX},
        {"ISO", NDB::FormatSettings::DateTimeFormat::ISO}
    };
    if (auto it = formats.find(formatName); it != formats.end()) {
        return it->second;
    }
    return NDB::FormatSettings::DateTimeFormat::Unspecified;
}

NDB::FormatSettings::TimestampFormat ToTimestampFormat(const TString& formatName) {
    static TMap<TString, NDB::FormatSettings::TimestampFormat> formats{
        {"POSIX", NDB::FormatSettings::TimestampFormat::POSIX},
        {"ISO", NDB::FormatSettings::TimestampFormat::ISO},
        {"UNIX_TIME_MILLISECONDS", NDB::FormatSettings::TimestampFormat::UnixTimeMilliseconds},
        {"UNIX_TIME_SECONDS", NDB::FormatSettings::TimestampFormat::UnixTimeSeconds},
        {"UNIX_TIME_MICROSECONDS", NDB::FormatSettings::TimestampFormat::UnixTimeMicroSeconds}
    };
    if (auto it = formats.find(formatName); it != formats.end()) {
        return it->second;
    }
    return NDB::FormatSettings::TimestampFormat::Unspecified;
}

} // namespace

using namespace NKikimr::NMiniKQL;

std::pair<NYql::NDq::IDqComputeActorAsyncInput*, IActor*> CreateS3ReadActor(
    const TTypeEnvironment& typeEnv,
    const THolderFactory& holderFactory,
    IHTTPGateway::TPtr gateway,
    NS3::TSource&& params,
    ui64 inputIndex,
    const TTxId& txId,
    const THashMap<TString, TString>& secureParams,
    const THashMap<TString, TString>& taskParams,
    const NActors::TActorId& computeActorId,
    ISecuredServiceAccountCredentialsFactory::TPtr credentialsFactory,
    const IRetryPolicy<long>::TPtr& retryPolicy,
    const TS3ReadActorFactoryConfig& cfg,
    IArrowReader::TPtr arrowReader,
    ::NMonitoring::TDynamicCounterPtr counters,
    ::NMonitoring::TDynamicCounterPtr taskCounters)
{
    const IFunctionRegistry& functionRegistry = *holderFactory.GetFunctionRegistry();

    TPathList paths;
    ui64 startPathIndex = 0;
    ReadPathsList(params, taskParams, paths, startPathIndex);

    const auto token = secureParams.Value(params.GetToken(), TString{});
    const auto credentialsProviderFactory = CreateCredentialsProviderFactoryForStructuredToken(credentialsFactory, token);
    const auto authToken = credentialsProviderFactory->CreateProvider()->GetAuthInfo();

    const auto& settings = params.GetSettings();
    TString pathPattern = "*";
    ES3PatternVariant pathPatternVariant = ES3PatternVariant::FilePattern;
    auto hasDirectories = std::find_if(paths.begin(), paths.end(), [](const TPath& a) {
                              return a.IsDirectory;
                          }) != paths.end();
    if (hasDirectories) {
        auto pathPatternValue = settings.find("pathpattern");
        if (pathPatternValue == settings.cend()) {
            ythrow yexception() << "'pathpattern' must be configured for directory listing";
        }
        pathPattern = pathPatternValue->second;

        auto pathPatternVariantValue = settings.find("pathpatternvariant");
        if (pathPatternVariantValue == settings.cend()) {
            ythrow yexception()
                << "'pathpatternvariant' must be configured for directory listing";
        }
        auto maybePathPatternVariant =
            NS3Lister::DeserializePatternVariant(pathPatternVariantValue->second);
        if (maybePathPatternVariant.Empty()) {
            ythrow yexception()
                << "Unknown 'pathpatternvariant': " << pathPatternVariantValue->second;
        }
        pathPatternVariant = *maybePathPatternVariant;
    }
    ui64 fileSizeLimit = cfg.FileSizeLimit;
    if (params.HasFormat()) {
        if (auto it = cfg.FormatSizeLimits.find(params.GetFormat()); it != cfg.FormatSizeLimits.end()) {
            fileSizeLimit = it->second;
        }
    }

    bool addPathIndex = false;
    if (auto it = settings.find("addPathIndex"); it != settings.cend()) {
        addPathIndex = FromString<bool>(it->second);
    }

    NYql::NSerialization::TSerializationInterval::EUnit intervalUnit = NYql::NSerialization::TSerializationInterval::EUnit::MICROSECONDS;
    if (auto it = settings.find("data.interval.unit"); it != settings.cend()) {
        intervalUnit = NYql::NSerialization::TSerializationInterval::ToUnit(it->second);
    }

    if (params.HasFormat() && params.HasRowType()) {
        const auto pb = std::make_unique<TProgramBuilder>(typeEnv, functionRegistry);
        const auto outputItemType = NCommon::ParseTypeFromYson(TStringBuf(params.GetRowType()), *pb, Cerr);
        const auto structType = static_cast<TStructType*>(outputItemType);

        const auto readSpec = std::make_shared<TReadSpec>();
        readSpec->Arrow = params.GetArrow();
        if (readSpec->Arrow) {
            arrow::SchemaBuilder builder;
            auto extraStructType = static_cast<TStructType*>(pb->NewStructType(structType, "_yql_block_length",
                pb->NewBlockType(pb->NewDataType(NUdf::EDataSlot::Uint64), TBlockType::EShape::Scalar)));

            for (ui32 i = 0U; i < extraStructType->GetMembersCount(); ++i) {
                if (extraStructType->GetMemberName(i) == "_yql_block_length") {
                    readSpec->BlockLengthPosition = i;
                    continue;
                }

                auto memberType = extraStructType->GetMemberType(i);
                std::shared_ptr<arrow::DataType> dataType;

                YQL_ENSURE(ConvertArrowType(memberType, dataType), "Unsupported arrow type");
                THROW_ARROW_NOT_OK(builder.AddField(std::make_shared<arrow::Field>(std::string(extraStructType->GetMemberName(i)), dataType, memberType->IsOptional())));
                readSpec->ColumnReorder.push_back(i);
            }

            auto res = builder.Finish();
            THROW_ARROW_NOT_OK(res.status());
            readSpec->ArrowSchema = std::move(res).ValueOrDie();
        } else {
            readSpec->CHColumns.resize(structType->GetMembersCount());
            for (ui32 i = 0U; i < structType->GetMembersCount(); ++i) {
                auto& column = readSpec->CHColumns[i];
                column.type = MetaToClickHouse(structType->GetMemberType(i), intervalUnit);
                column.name = structType->GetMemberName(i);
            }
        }

        readSpec->Format = params.GetFormat();

        if (const auto it = settings.find("compression"); settings.cend() != it)
            readSpec->Compression = it->second;

        if (const auto it = settings.find("csvdelimiter"); settings.cend() != it && !it->second.empty())
            readSpec->Settings.csv.delimiter = it->second[0];

        if (const auto it = settings.find("data.datetime.formatname"); settings.cend() != it) {
            readSpec->Settings.date_time_format_name = ToDateTimeFormat(it->second);
        }

        if (const auto it = settings.find("data.datetime.format"); settings.cend() != it) {
            readSpec->Settings.date_time_format = it->second;
        }

        if (const auto it = settings.find("data.timestamp.formatname"); settings.cend() != it) {
            readSpec->Settings.timestamp_format_name = ToTimestampFormat(it->second);
        }

        if (const auto it = settings.find("data.timestamp.format"); settings.cend() != it) {
            readSpec->Settings.timestamp_format = it->second;
        }

        if (readSpec->Settings.date_time_format_name == NDB::FormatSettings::DateTimeFormat::Unspecified && readSpec->Settings.date_time_format.empty()) {
            readSpec->Settings.date_time_format_name = NDB::FormatSettings::DateTimeFormat::POSIX;
        }

        if (readSpec->Settings.timestamp_format_name == NDB::FormatSettings::TimestampFormat::Unspecified && readSpec->Settings.timestamp_format.empty()) {
            readSpec->Settings.timestamp_format_name = NDB::FormatSettings::TimestampFormat::POSIX;
        }

#define SUPPORTED_FLAGS(xx) \
        xx(skip_unknown_fields, true) \
        xx(import_nested_json, true) \
        xx(with_names_use_header, true) \
        xx(null_as_default, true) \

#define SET_FLAG(flag, def) \
        if (const auto it = settings.find(#flag); settings.cend() != it) \
            readSpec->Settings.flag = FromString<bool>(it->second); \
        else \
            readSpec->Settings.flag = def;

        SUPPORTED_FLAGS(SET_FLAG)

#undef SET_FLAG
#undef SUPPORTED_FLAGS
        std::size_t maxBlocksInFly = 2;
        if (const auto it = settings.find("fileReadBlocksInFly"); settings.cend() != it)
            maxBlocksInFly = FromString<ui64>(it->second);
        const auto actor = new TS3StreamReadActor(inputIndex, txId, std::move(gateway), holderFactory, params.GetUrl(), authToken, pathPattern, pathPatternVariant,
                                                  std::move(paths), addPathIndex, startPathIndex, readSpec, computeActorId, retryPolicy,
                                                  maxBlocksInFly, arrowReader, cfg, counters, taskCounters, fileSizeLimit);

        return {actor, actor};
    } else {
        ui64 sizeLimit = std::numeric_limits<ui64>::max();
        if (const auto it = settings.find("sizeLimit"); settings.cend() != it)
            sizeLimit = FromString<ui64>(it->second);

        const auto actor = new TS3ReadActor(inputIndex, txId, std::move(gateway), holderFactory, params.GetUrl(), authToken, pathPattern, pathPatternVariant,
                                            std::move(paths), addPathIndex, startPathIndex, computeActorId, sizeLimit, retryPolicy,
                                            cfg, counters, taskCounters, fileSizeLimit);
        return {actor, actor};
    }
}

} // namespace NYql::NDq
