// Copyright 2014 Cloudera, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
package org.kududb.client;

import org.junit.Test;

import static org.junit.Assert.*;

public class TestKuduSession extends BaseKuduTest {
  // Generate a unique table name
  private static final String TABLE_NAME_PREFIX =
      TestKuduSession.class.getName()+"-"+System.currentTimeMillis();

  private KuduTable table;

  @Test(timeout = 100000)
  public void testBasicOps() throws Exception {
    String tableName = TABLE_NAME_PREFIX + "-testBasicOps";
    table = createTable(tableName, basicSchema, new CreateTableBuilder());

    KuduSession session = syncClient.newSession();
    for (int i = 0; i < 10; i++) {
      session.apply(createInsert(i));
    }
    assertEquals(10, countRowsInScan(client.newScannerBuilder(table).build()));

    OperationResponse resp = session.apply(createInsert(0));
    assertTrue(resp.hasRowError());

    session.setFlushMode(SessionConfiguration.FlushMode.MANUAL_FLUSH);

    for (int i = 10; i < 20; i++) {
      session.apply(createInsert(i));
    }
    session.flush();
    assertEquals(20, countRowsInScan(client.newScannerBuilder(table).build()));
  }

  @Test(timeout = 100000)
  public void testBatchWithSameRow() throws Exception {
    String tableName = TABLE_NAME_PREFIX + "-testBatchWithSameRow";
    table = createTable(tableName, basicSchema, new CreateTableBuilder());

    KuduSession session = syncClient.newSession();
    session.setFlushMode(SessionConfiguration.FlushMode.MANUAL_FLUSH);

    // Insert 25 rows, one per batch, along with 50 updates for each, and a delete at the end,
    // while also clearing the cache between each batch half the time. The delete is added here
    // so that a misplaced update would fail if it happens later than its delete.
    for (int i = 0; i < 25; i++) {
      session.apply(createInsert(i));
      for (int j = 0; j < 50; j++) {
        Update update = table.newUpdate();
        PartialRow row = update.getRow();
        row.addInt(basicSchema.getColumnByIndex(0).getName(), i);
        row.addInt(basicSchema.getColumnByIndex(1).getName(), 1000);
        session.apply(update);
      }
      Delete del = table.newDelete();
      PartialRow row = del.getRow();
      row.addInt(basicSchema.getColumnByIndex(0).getName(), i);
      session.apply(del);
      session.flush();
      if (i % 2 == 0) {
        client.emptyTabletsCacheForTable(table.getTableId());
      }
    }
    assertEquals(0, countRowsInScan(client.newScannerBuilder(table).build()));
  }

  /**
   * Regression test for KUDU-1226. Calls to session.flush() concurrent with AUTO_FLUSH_BACKGROUND
   * can end up giving ConvertBatchToListOfResponsesCB a list with nulls if a tablet was already
   * flushed. Only happens with multiple tablets.
   */
  @Test(timeout = 10000)
  public void testConcurrentFlushes() throws Exception {
    String tableName = TABLE_NAME_PREFIX + "-testConcurrentFlushes";
    CreateTableBuilder builder = new CreateTableBuilder();
    int numTablets = 4;
    int numRowsPerTablet = 100;

    // Create a 4 tablets table split on 1000, 2000, and 3000.
    for (int i = 1; i < numTablets; i++) {
      PartialRow split = basicSchema.newPartialRow();
      split.addInt(0, i * numRowsPerTablet);
      builder.addSplitRow(split);
    }
    table = createTable(tableName, basicSchema, builder);

    // Configure the session to background flush as often as it can (every 1ms).
    KuduSession session = syncClient.newSession();
    session.setFlushMode(SessionConfiguration.FlushMode.AUTO_FLUSH_BACKGROUND);
    session.setFlushInterval(1);

    // Fill each tablet in parallel 1 by 1 then flush. Without the fix this would quickly get an
    // NPE.
    for (int i = 0; i < numRowsPerTablet; i++) {
      for (int j = 0; j < numTablets; j++) {
        session.apply(createInsert(i + (numRowsPerTablet * j)));
      }
      session.flush();
    }
  }

  private Insert createInsert(int key) {
    return createBasicSchemaInsert(table, key);
  }
}
