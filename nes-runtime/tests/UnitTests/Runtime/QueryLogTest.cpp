/*
    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        https://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <barrier>
#include <chrono>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <Identifiers/Identifiers.hpp>
#include <Listeners/QueryLog.hpp>
#include <Runtime/Execution/QueryStatus.hpp>
#include <ErrorHandling.hpp>

using namespace std::chrono_literals;

namespace NES
{

class QueryLogTest : public ::testing::Test
{
protected:
    void SetUp() override { queryLog = std::make_unique<QueryLog>(); }

    std::unique_ptr<QueryLog> queryLog;
    const QueryId testQueryId{42};
    const std::chrono::system_clock::time_point testTime = std::chrono::system_clock::now();
};

/// NOLINTBEGIN(readability-magic-numbers,bugprone-unchecked-optional-access,misc-include-cleaner)
TEST_F(QueryLogTest, LogQueryStatusChangeBasic)
{
    EXPECT_TRUE(queryLog->logQueryStatusChange(testQueryId, QueryState::Compiling, testTime));
    EXPECT_TRUE(queryLog->logQueryStatusChange(testQueryId, QueryState::Started, testTime + 100ms));
    EXPECT_TRUE(queryLog->logQueryStatusChange(testQueryId, QueryState::Running, testTime + 200ms));
    EXPECT_TRUE(queryLog->logQueryStatusChange(testQueryId, QueryState::Stopped, testTime + 300ms));
}

TEST_F(QueryLogTest, GetLogForQuery)
{
    queryLog->logQueryStatusChange(testQueryId, QueryState::Compiling, testTime);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Started, testTime + 100ms);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Running, testTime + 200ms);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Stopped, testTime + 300ms);

    const auto log = queryLog->getLogForQuery(testQueryId);
    ASSERT_TRUE(log.has_value());
    EXPECT_EQ(log->size(), 4);

    EXPECT_EQ(log->at(0).state, QueryState::Compiling);
    EXPECT_EQ(log->at(1).state, QueryState::Started);
    EXPECT_EQ(log->at(2).state, QueryState::Running);
    EXPECT_EQ(log->at(3).state, QueryState::Stopped);
}

TEST_F(QueryLogTest, GetLogForNonExistentQuery)
{
    const auto log = queryLog->getLogForQuery(QueryId{999});
    EXPECT_FALSE(log.has_value());
}

TEST_F(QueryLogTest, GetQuerySummarySuccessfulExecution)
{
    queryLog->logQueryStatusChange(testQueryId, QueryState::Compiling, testTime);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Started, testTime + 100ms);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Running, testTime + 200ms);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Stopped, testTime + 300ms);

    const auto status = queryLog->getQueryStatus(testQueryId);
    ASSERT_TRUE(status.has_value());

    EXPECT_EQ(status->queryId, testQueryId);
    EXPECT_EQ(status->state, QueryState::Stopped);

    EXPECT_TRUE(status->metrics.compiling.has_value());
    EXPECT_TRUE(status->metrics.start.has_value());
    EXPECT_TRUE(status->metrics.running.has_value());
    EXPECT_TRUE(status->metrics.stop.has_value());
    EXPECT_FALSE(status->metrics.error.has_value());

    EXPECT_EQ(*status->metrics.compiling, testTime);
    EXPECT_EQ(*status->metrics.start, testTime + 100ms);
    EXPECT_EQ(*status->metrics.running, testTime + 200ms);
    EXPECT_EQ(*status->metrics.stop, testTime + 300ms);
}

TEST_F(QueryLogTest, GetQuerySummaryWithCompilationPhase)
{
    queryLog->logQueryStatusChange(testQueryId, QueryState::Started, testTime);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Compiling, testTime + 25ms);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Running, testTime + 100ms);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Stopped, testTime + 200ms);

    const auto status = queryLog->getQueryStatus(testQueryId);
    ASSERT_TRUE(status.has_value());

    EXPECT_EQ(status->queryId, testQueryId);
    EXPECT_EQ(status->state, QueryState::Stopped);

    EXPECT_TRUE(status->metrics.start.has_value());
    EXPECT_TRUE(status->metrics.compiling.has_value());
    EXPECT_TRUE(status->metrics.running.has_value());
    EXPECT_TRUE(status->metrics.stop.has_value());
    EXPECT_FALSE(status->metrics.error.has_value());

    EXPECT_EQ(*status->metrics.start, testTime);
    EXPECT_EQ(*status->metrics.compiling, testTime + 25ms);
    EXPECT_EQ(*status->metrics.running, testTime + 100ms);
    EXPECT_EQ(*status->metrics.stop, testTime + 200ms);
}

TEST_F(QueryLogTest, GetQuerySummaryWithFailure)
{
    const Exception testError{"Test error", 500};

    queryLog->logQueryStatusChange(testQueryId, QueryState::Compiling, testTime);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Started, testTime + 100ms);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Running, testTime + 200ms);
    queryLog->logQueryFailure(testQueryId, testError, testTime + 300ms);

    auto summary = queryLog->getQueryStatus(testQueryId);
    ASSERT_TRUE(summary.has_value());

    EXPECT_EQ(summary->queryId, testQueryId);
    EXPECT_EQ(summary->state, QueryState::Failed);

    EXPECT_TRUE(summary->metrics.compiling.has_value());
    EXPECT_TRUE(summary->metrics.start.has_value());
    EXPECT_TRUE(summary->metrics.running.has_value());
    EXPECT_TRUE(summary->metrics.stop.has_value());
    EXPECT_TRUE(summary->metrics.error.has_value());

    EXPECT_EQ(summary->metrics.error->code(), 500);
    EXPECT_EQ(summary->metrics.error->what(), "Test error");
}

TEST_F(QueryLogTest, GetQuerySummaryDuringCompilation)
{
    queryLog->logQueryStatusChange(testQueryId, QueryState::Compiling, testTime);

    const auto status = queryLog->getQueryStatus(testQueryId);
    ASSERT_TRUE(status.has_value());

    EXPECT_EQ(status->queryId, testQueryId);
    EXPECT_EQ(status->state, QueryState::Compiling);

    EXPECT_TRUE(status->metrics.compiling.has_value());
    EXPECT_FALSE(status->metrics.start.has_value());
    EXPECT_FALSE(status->metrics.running.has_value());
    EXPECT_FALSE(status->metrics.stop.has_value());
    EXPECT_FALSE(status->metrics.error.has_value());
}

TEST_F(QueryLogTest, GetQuerySummaryPartialExecution)
{
    queryLog->logQueryStatusChange(testQueryId, QueryState::Compiling, testTime);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Started, testTime + 100ms);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Running, testTime + 200ms);

    const auto status = queryLog->getQueryStatus(testQueryId);
    ASSERT_TRUE(status.has_value());

    EXPECT_EQ(status->queryId, testQueryId);
    EXPECT_EQ(status->state, QueryState::Running);

    EXPECT_TRUE(status->metrics.compiling.has_value());
    EXPECT_TRUE(status->metrics.start.has_value());
    EXPECT_TRUE(status->metrics.running.has_value());
    EXPECT_FALSE(status->metrics.stop.has_value());
    EXPECT_FALSE(status->metrics.error.has_value());
}

TEST_F(QueryLogTest, GetQuerySummaryForNonExistentQuery)
{
    const auto status = queryLog->getQueryStatus(QueryId{999});
    EXPECT_FALSE(status.has_value());
}

TEST_F(QueryLogTest, MultipleQueriesIndependentLogs)
{
    constexpr QueryId query1{1};
    constexpr QueryId query2{2};

    queryLog->logQueryStatusChange(query1, QueryState::Compiling, testTime);
    queryLog->logQueryStatusChange(query1, QueryState::Started, testTime + 25ms);
    queryLog->logQueryStatusChange(query2, QueryState::Compiling, testTime + 50ms);
    queryLog->logQueryStatusChange(query2, QueryState::Started, testTime + 75ms);
    queryLog->logQueryStatusChange(query1, QueryState::Running, testTime + 100ms);
    queryLog->logQueryFailure(query2, Exception{"Query 2 failed", 400}, testTime + 150ms);

    const auto status1 = queryLog->getQueryStatus(query1);
    const auto status2 = queryLog->getQueryStatus(query2);

    ASSERT_TRUE(status1.has_value());
    ASSERT_TRUE(status2.has_value());

    EXPECT_EQ(status1->state, QueryState::Running);
    EXPECT_EQ(status2->state, QueryState::Failed);

    EXPECT_TRUE(status1->metrics.compiling.has_value());
    EXPECT_FALSE(status1->metrics.error.has_value());
    EXPECT_TRUE(status2->metrics.compiling.has_value());
    EXPECT_TRUE(status2->metrics.error.has_value());
    EXPECT_EQ(status2->metrics.error->code(), 400);
}

TEST_F(QueryLogTest, QueryStateChangeConstructors)
{
    const QueryStateChange stateChange1(QueryState::Running, testTime);
    EXPECT_EQ(stateChange1.state, QueryState::Running);
    EXPECT_EQ(stateChange1.timestamp, testTime);
    EXPECT_FALSE(stateChange1.exception.has_value());

    const Exception testError{"Test exception", 404};
    QueryStateChange stateChange2(testError, testTime);
    EXPECT_EQ(stateChange2.state, QueryState::Failed);
    EXPECT_EQ(stateChange2.timestamp, testTime);
    EXPECT_TRUE(stateChange2.exception.has_value());
    EXPECT_EQ(stateChange2.exception->code(), 404);
}

TEST_F(QueryLogTest, OutOfOrderEventsWithMonotonicTimestamps)
{
    const auto time0 = testTime;
    const auto time1 = testTime + 100ms;
    const auto time2 = testTime + 200ms;
    const auto time3 = testTime + 300ms;

    queryLog->logQueryStatusChange(testQueryId, QueryState::Running, time2);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Started, time1);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Compiling, time0);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Stopped, time3);

    const auto log = queryLog->getLogForQuery(testQueryId);
    ASSERT_TRUE(log.has_value());
    EXPECT_EQ(log->size(), 4);

    const auto status = queryLog->getQueryStatus(testQueryId);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->state, QueryState::Stopped);

    EXPECT_TRUE(status->metrics.compiling.has_value());
    EXPECT_TRUE(status->metrics.start.has_value());
    EXPECT_TRUE(status->metrics.running.has_value());
    EXPECT_TRUE(status->metrics.stop.has_value());

    EXPECT_EQ(*status->metrics.compiling, time0);
    EXPECT_EQ(*status->metrics.start, time1);
    EXPECT_EQ(*status->metrics.running, time2);
    EXPECT_EQ(*status->metrics.stop, time3);
}

TEST_F(QueryLogTest, EventsWithEqualTimestamps)
{
    const auto sameTime = testTime;
    const Exception testError{"Test failure", 500};

    queryLog->logQueryStatusChange(testQueryId, QueryState::Registered, sameTime);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Compiling, sameTime);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Started, sameTime);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Running, sameTime);
    queryLog->logQueryStatusChange(testQueryId, QueryState::Stopped, sameTime);
    queryLog->logQueryFailure(testQueryId, testError, sameTime);

    const auto log = queryLog->getLogForQuery(testQueryId);
    ASSERT_TRUE(log.has_value());
    EXPECT_EQ(log->size(), 6);

    for (const auto& entry : *log)
    {
        EXPECT_EQ(entry.timestamp, sameTime);
    }

    const auto status = queryLog->getQueryStatus(testQueryId);
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->state, QueryState::Failed);

    EXPECT_EQ(*status->metrics.compiling, sameTime);
    EXPECT_EQ(*status->metrics.start, sameTime);
    EXPECT_EQ(*status->metrics.running, sameTime);
    EXPECT_EQ(*status->metrics.stop, sameTime);

    EXPECT_TRUE(status->metrics.error.has_value());
    EXPECT_EQ(status->metrics.error->code(), 500);
    EXPECT_STREQ(status->metrics.error->what(), "Test failure");
}

TEST_F(QueryLogTest, MultiThreadedLogging)
{
    constexpr uint64_t numThreads = 16;
    constexpr uint64_t numQueries = 1'000;
    const auto baseTime = testTime;

    std::barrier barrier{numThreads};

    const auto logSubmission = [this, baseTime, &barrier]
    {
        barrier.arrive_and_wait();
        for (uint64_t queryId = 0; queryId < numQueries; ++queryId)
        {
            const auto timestamp = baseTime + std::chrono::milliseconds{queryId * 10};

            queryLog->logQueryStatusChange(QueryId{queryId}, QueryState::Registered, timestamp);
            queryLog->logQueryStatusChange(QueryId{queryId}, QueryState::Compiling, timestamp);
            queryLog->logQueryStatusChange(QueryId{queryId}, QueryState::Started, timestamp);
            queryLog->logQueryStatusChange(QueryId{queryId}, QueryState::Running, timestamp);
            queryLog->logQueryStatusChange(QueryId{queryId}, QueryState::Stopped, timestamp);
            queryLog->logQueryFailure(QueryId{queryId}, Exception{"Test failure", 404}, timestamp);
            queryLog->logQueryStatusChange(QueryId{queryId}, QueryState::Running, timestamp);
        }
    };

    {
        std::vector<std::jthread> threads;
        threads.reserve(numThreads);
        for (uint64_t threadId = 0; threadId < numThreads; ++threadId)
        {
            threads.emplace_back(logSubmission);
        }
    }

    constexpr uint64_t eventsPerQuery = numThreads * 7;
    for (uint64_t queryId = 0; queryId < numThreads; ++queryId)
    {
        const auto log = queryLog->getLogForQuery(QueryId{queryId});
        ASSERT_TRUE(log.has_value()) << "Query " << queryId << " events not found";
        EXPECT_EQ(log->size(), eventsPerQuery) << "Query " << queryId << " wrong event count";

        const auto status = queryLog->getQueryStatus(QueryId{queryId});
        ASSERT_TRUE(status.has_value());
        EXPECT_EQ(status->state, QueryState::Failed);
        EXPECT_TRUE(status->metrics.compiling.has_value());
        EXPECT_TRUE(status->metrics.start.has_value());
        EXPECT_TRUE(status->metrics.stop.has_value());
        EXPECT_TRUE(status->metrics.error.has_value());
        EXPECT_EQ(status->metrics.error->code(), 404);
        EXPECT_STREQ(status->metrics.error->what(), "Test failure");
    }

    const auto statusResults = queryLog->getStatus();
    EXPECT_EQ(statusResults.size(), numQueries);
}

/// NOLINTEND(readability-magic-numbers,bugprone-unchecked-optional-access,misc-include-cleaner)
}
