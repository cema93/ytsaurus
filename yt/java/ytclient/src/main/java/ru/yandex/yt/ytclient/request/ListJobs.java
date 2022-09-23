package ru.yandex.yt.ytclient.request;

import java.util.Objects;

import javax.annotation.Nullable;

import ru.yandex.inside.yt.kosher.common.GUID;
import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.rpcproxy.EJobState;
import ru.yandex.yt.rpcproxy.TReqListJobs;
import ru.yandex.yt.ytclient.proxy.request.HighLevelRequest;
import ru.yandex.yt.ytclient.rpc.RpcClientRequestBuilder;
import ru.yandex.yt.ytclient.rpc.RpcUtil;

public class ListJobs extends RequestBase<ListJobs.Builder> implements HighLevelRequest<TReqListJobs.Builder> {
    private final GUID operationId;
    @Nullable
    private final JobState state;
    @Nullable
    private final Long limit;

    ListJobs(Builder builder) {
        super(builder);
        this.operationId = Objects.requireNonNull(builder.operationId);
        this.state = builder.state;
        this.limit = builder.limit;
    }

    public static Builder builder() {
        return new Builder();
    }

    @Override
    public Builder toBuilder() {
        Builder builder = builder().setOperationId(operationId);
        if (state != null) {
            builder.setState(state);
        }
        if (limit != null)  {
            builder.setLimit(limit);
        }
        builder.setTimeout(timeout)
                .setRequestId(requestId)
                .setUserAgent(userAgent)
                .setTraceId(traceId, traceSampled)
                .setAdditionalData(additionalData);
        return builder;
    }

    @Override
    public void writeTo(RpcClientRequestBuilder<TReqListJobs.Builder, ?> requestBuilder) {
        requestBuilder.body().setOperationId(RpcUtil.toProto(operationId));
        if (state != null) {
            requestBuilder.body().setState(EJobState.forNumber(state.getProtoValue()));
        }
        if (limit != null) {
            requestBuilder.body().setLimit(limit);
        }
    }

    @Override
    protected void writeArgumentsLogString(StringBuilder sb) {
        sb.append("OperationId: ").append(operationId).append("; ");
        if (state != null) {
            sb.append("State: ").append(state.getWireName()).append("; ");
        }
        if (limit != null) {
            sb.append("Limit: ").append(limit).append("; ");
        }
        super.writeArgumentsLogString(sb);
    }

    @NonNullApi
    @NonNullFields
    public static class Builder extends RequestBase.Builder<Builder> {
        @Nullable
        private GUID operationId;
        @Nullable
        private JobState state;
        @Nullable
        private Long limit;

        public Builder() {
        }

        public Builder(Builder builder) {
            super(builder);
            operationId = builder.operationId;
            state = builder.state;
            limit = builder.limit;
        }

        public Builder setOperationId(GUID operationId) {
            this.operationId = operationId;
            return self();
        }

        public Builder setState(JobState state) {
            this.state = state;
            return self();
        }

        public Builder setLimit(Long limit) {
            this.limit = limit;
            return self();
        }

        public ListJobs build() {
            return new ListJobs(this);
        }

        @Override
        protected Builder self() {
            return this;
        }
    }
}