// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
package org.yb.cql;

import com.datastax.driver.core.ResultSet;
import com.datastax.driver.core.Row;
import org.junit.Test;

import java.util.*;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.assertEquals;

public class TestCollectionExpressions extends BaseCQLTest {
  private String createTableStmt(String tableName, String keyType, String elemType)
      throws Exception {
    return String.format("CREATE TABLE %s (h int, r int, " +
        "vm map<%2$s, %3$s>, vs set<%2$s>, vl list<%3$s>," +
        "primary key((h), r));", tableName, keyType, elemType);
  }

  @Test
  public void testPlusExpressions() throws Exception {

    //--------------------- Setting up for Insert and Update tests ---------------------------------
    String tableName = "test_coll_exp";

    String createStmt = createTableStmt(tableName, "int", "text");
    session.execute(createStmt);

    String insert_template = "INSERT INTO " + tableName +
        " (h, r, vm, vs, vl) VALUES (%d, %d, %s, %s, %s);";
    session.execute(String.format(insert_template, 1, 1, "{2 : 'b', 3: 'c'}",
        "{1, 2}", "['x', 'y']"));
    session.execute(String.format(insert_template, 1, 2, "{}", "{}", "[]"));

    String select_template = "SELECT * FROM " + tableName + " WHERE h = %d AND r = %d";

    //----------------------------------------------------------------------------------------------
    // Testing Map Extend: vm = vm + <value>
    //----------------------------------------------------------------------------------------------
    {
      String update_template = "UPDATE " + tableName + " SET vm = vm + %s " +
          "WHERE h = %d AND r = %d";

      //-------------------------------- Valid Statements ------------------------------------------

      // Test add/update existing value.
      session.execute(String.format(update_template, "{1 : 'a', 3 : 'c1'}", 1, 1));
      // Checking row -- expecting key 1 is added (val "a"), key 3 is overwritten (to "c1")
      Row row = runSelect(String.format(select_template, 1, 1)).next();
      Map map = row.getMap("vm", Integer.class, String.class);
      assertEquals(3, map.size());
      assertEquals("a", map.get(1));
      assertEquals("b", map.get(2));
      assertEquals("c1", map.get(3));

      // Test extending null column with null value.
      session.execute(String.format(update_template, "{ }", 1, 2));
      // Checking row
      row = runSelect(String.format(select_template, 1, 2)).next();
      assertTrue(row.isNull("vm"));

      // Test extending null column.
      session.execute(String.format(update_template, "{11 : 'x', 22 : 'y'}", 1, 2));
      // Checking row
      row = runSelect(String.format(select_template, 1, 2)).next();
      map = row.getMap("vm", Integer.class, String.class);
      assertEquals(2, map.size());
      assertEquals("x", map.get(11));
      assertEquals("y", map.get(22));

      // Test extending with null value.
      session.execute(String.format(update_template, "{ }", 1, 2));
      // Checking row
      row = runSelect(String.format(select_template, 1, 2)).next();
      map = row.getMap("vm", Integer.class, String.class);
      assertEquals(2, map.size());
      assertEquals("x", map.get(11));
      assertEquals("y", map.get(22));

      //------------------------------- Invalid Statements -----------------------------------------

      // Test wrong collection type.
      String invalidStmt =
          "UPDATE " + tableName + " SET vm = vm + {'a', 'b'} WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test wrong keys type.
      invalidStmt =
          "UPDATE " + tableName + " SET vm = vm + {2.5 : 'a'} WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test wrong values type.
      invalidStmt = "UPDATE " + tableName + " SET vm = vm + {2 : 5} WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid arguments: wrong order.
      invalidStmt = "UPDATE " + tableName + " SET vm = {2 : 'a'} + vm WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid arguments: two literals
      invalidStmt =
          "UPDATE " + tableName + " SET vm = {1 : 'a'} + {2 : 'b'} WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid arguments: two column refs
      invalidStmt = "UPDATE " + tableName + " SET vm = vm + vm WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);
    }

    //----------------------------------------------------------------------------------------------
    // Testing Set Extend: vs = vs + <value>
    //----------------------------------------------------------------------------------------------
    {
      String update_template = "UPDATE " + tableName + " SET vs = vs + %s " +
          "WHERE h = %d AND r = %d";

      // Test add/update existing value.
      session.execute(String.format(update_template, "{0, 2, 3}", 1, 1));
      // Checking row
      Row row = runSelect(String.format(select_template, 1, 1)).next();
      Set set = row.getSet("vs", Integer.class);
      assertEquals(4, set.size());
      assertTrue(set.contains(0));
      assertTrue(set.contains(1));
      assertTrue(set.contains(2));
      assertTrue(set.contains(3));

      // Test extending null column with null value.
      session.execute(String.format(update_template, "{ }", 1, 2));
      // Checking row
      row = runSelect(String.format(select_template, 1, 2)).next();
      assertTrue(row.isNull("vs"));

      // Test extending null column.
      session.execute(String.format(update_template, "{11, 22}", 1, 2));
      // Checking row
      row = runSelect(String.format(select_template, 1, 2)).next();
      set = row.getSet("vs", Integer.class);
      assertEquals(2, set.size());
      assertTrue(set.contains(11));
      assertTrue(set.contains(22));

      // Test extending with snull value.
      session.execute(String.format(update_template, "{ }", 1, 2));
      // Checking row
      row = runSelect(String.format(select_template, 1, 2)).next();
      set = row.getSet("vs", Integer.class);
      assertEquals(2, set.size());
      assertTrue(set.contains(11));
      assertTrue(set.contains(22));

      //------------------------------- Invalid Statements -----------------------------------------

      // Test wrong collection type.
      String invalidStmt =
          "UPDATE " + tableName + " SET vs = vs + [2, 3] WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test wrong elems type.
      invalidStmt = "UPDATE " + tableName + " SET vs = vs + {2.5} WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid argument: wrong order.
      invalidStmt = "UPDATE " + tableName + " SET vs = {'a', 'b'} + vs WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid arguments: two literals
      invalidStmt = "UPDATE " + tableName + " SET vl = {'a'} + {'v'} WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid arguments: two column refs
      invalidStmt = "UPDATE " + tableName + " SET vs = vs + vs WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);
    }

    //----------------------------------------------------------------------------------------------
    // Testing List Append: vl = vl + <value>
    //----------------------------------------------------------------------------------------------
    {
      String update_template = "UPDATE " + tableName + " SET vl = vl + %s " +
          "WHERE h = %d AND r = %d";

      // Test add/update existing value.
      session.execute(String.format(update_template, "['a', 'b']", 1, 1));
      // Checking row
      Row row = runSelect(String.format(select_template, 1, 1)).next();
      List list = row.getList("vl", String.class);
      assertEquals(4, list.size());
      assertEquals("x", list.get(0));
      assertEquals("y", list.get(1));
      assertEquals("a", list.get(2));
      assertEquals("b", list.get(3));

      // Test appending null column with null value.
      session.execute(String.format(update_template, "[ ]", 1, 2));
      // Checking row
      row = runSelect(String.format(select_template, 1, 2)).next();
      assertTrue(row.isNull("vl"));

      // Test appending to null column.
      session.execute(String.format(update_template, "['bb', 'aa']", 1, 2));
      // Checking row
      row = runSelect(String.format(select_template, 1, 2)).next();
      list = row.getList("vl", String.class);
      assertEquals(2, list.size());
      assertEquals("bb", list.get(0));
      assertEquals("aa", list.get(1));

      // Test appending with null value.
      session.execute(String.format(update_template, "[ ]", 1, 2));
      // Checking row
      row = runSelect(String.format(select_template, 1, 2)).next();
      list = row.getList("vl", String.class);
      assertEquals(2, list.size());
      assertEquals("bb", list.get(0));
      assertEquals("aa", list.get(1));

      //------------------------------- Invalid Statements -----------------------------------------

      // Test wrong collection type.
      String invalidStmt =
          "UPDATE " + tableName + " SET vl = vl + {'b', 'a'} WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test wrong elems type.
      invalidStmt = "UPDATE " + tableName + " SET vl = vl + [2.5] WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid arguments: two literals
      invalidStmt = "UPDATE " + tableName + " SET vl = [2] + [3] WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid arguments: two column refs
      invalidStmt = "UPDATE " + tableName + " SET vl = vl + vl WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);
    }

    //----------------------------------------------------------------------------------------------
    // Testing List Prepend: vl = <value> + vl
    //----------------------------------------------------------------------------------------------
    {
      // Setting up.
      session.execute(String.format(insert_template, 2, 1, "{2 : 'b', 3: 'c'}",
          "{1, 2}", "['x', 'y']"));
      session.execute(String.format(insert_template, 2, 2, "{}", "{}", "[]"));
      String update_template = "UPDATE " + tableName + " SET vl = %s + vl " +
          "WHERE h = %d AND r = %d";

      // Test add/update existing value.
      session.execute(String.format(update_template, "['a', 'b']", 2, 1));
      // Checking row.
      Row row = runSelect(String.format(select_template, 2, 1)).next();
      List list = row.getList("vl", String.class);
      assertEquals(4, list.size());
      assertEquals("a", list.get(0));
      assertEquals("b", list.get(1));
      assertEquals("x", list.get(2));
      assertEquals("y", list.get(3));

      // Test prepending null column with null value.
      session.execute(String.format(update_template, "[ ]", 2, 2));
      // Checking row
      row = runSelect(String.format(select_template, 2, 2)).next();
      assertTrue(row.isNull("vl"));

      // Test prepending to null column.
      session.execute(String.format(update_template, "['bb', 'aa']", 2, 2));
      // Checking row
      row = runSelect(String.format(select_template, 2, 2)).next();
      list = row.getList("vl", String.class);
      assertEquals(2, list.size());
      assertEquals("bb", list.get(0));
      assertEquals("aa", list.get(1));

      // Test prepending with null value.
      session.execute(String.format(update_template, "[ ]", 2, 2));
      // Checking row
      row = runSelect(String.format(select_template, 2, 2)).next();
      list = row.getList("vl", String.class);
      assertEquals(2, list.size());
      assertEquals("bb", list.get(0));
      assertEquals("aa", list.get(1));

      //------------------------------- Invalid Statements -----------------------------------------

      // Test wrong collection type.
      String invalidStmt =
          "UPDATE " + tableName + " SET vl = {'b', 'a'} + vl WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test wrong elems type.
      invalidStmt = "UPDATE " + tableName + " SET vl = [2.5] + vl WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);
    }
  }

  @Test
  public void testMinusExpressions() throws Exception {

    //--------------------- Setting up for Insert and Update tests ---------------------------------
    String tableName = "test_coll_exp";

    String createStmt = createTableStmt(tableName, "int", "text");
    session.execute(createStmt);

    String insert_template = "INSERT INTO " + tableName +
        " (h, r, vm, vs, vl) VALUES (%d, %d, %s, %s, %s);";
    session.execute(String.format(insert_template, 1, 1, "{2 : 'b', 3: 'c'}",
        "{1, 2}", "['x', 'y', 'x']"));
    session.execute(String.format(insert_template, 1, 2, "{}", "{}", "[]"));

    String select_template = "SELECT * FROM " + tableName + " WHERE h = %d AND r = %d";

    //----------------------------------------------------------------------------------------------
    // Testing Map Extend: vm = vm + <value>
    //----------------------------------------------------------------------------------------------
    {
      String update_template = "UPDATE " + tableName + " SET vm = vm - %s " +
          "WHERE h = %d AND r = %d";

      //-------------------------------- Valid Statements ------------------------------------------

      // Test removing existing and non-existing entries.
      session.execute(String.format(update_template, "{1, 3}", 1, 1));
      // Checking row -- removing key 1, ignoring key 3 since it does not exist
      Row row = runSelect(String.format(select_template, 1, 1)).next();
      Map map = row.getMap("vm", Integer.class, String.class);
      assertEquals(1, map.size());
      assertEquals("b", map.get(2));

      // Test removing null value from null column.
      session.execute(String.format(update_template, "{ }", 1, 2));
      // Checking row
      row = runSelect(String.format(select_template, 1, 2)).next();
      assertTrue(row.isNull("vm"));

      // Test removing from null column.
      session.execute(String.format(update_template, "{11, 12}", 1, 2));
      // Checking row
      row = runSelect(String.format(select_template, 1, 2)).next();
      map = row.getMap("vm", Integer.class, String.class);
      assertTrue(map.isEmpty());

      // Test removing a null value.
      session.execute(String.format(update_template, "{ }", 1, 1));
      // Checking row
      row = runSelect(String.format(select_template, 1, 1)).next();
      map = row.getMap("vm", Integer.class, String.class);
      assertEquals(1, map.size());
      assertEquals("b", map.get(2));

      //------------------------------- Invalid Statements -----------------------------------------

      // Test wrong collection type.
      String invalidStmt =
          "UPDATE " + tableName + " SET vm = vm - {1 : 'a'} WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test wrong keys type.
      invalidStmt = "UPDATE " + tableName + " SET vm = vm - {2.5} WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid arguments: wrong order.
      invalidStmt = "UPDATE " + tableName + " SET vm = {2 : 'a'} - vs WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid arguments: two literals
      invalidStmt =
          "UPDATE " + tableName + " SET vm = {1 : 'a'} - {2} WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid arguments: two column refs
      invalidStmt = "UPDATE " + tableName + " SET vm = vm - vs WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);
    }

    //----------------------------------------------------------------------------------------------
    // Testing Set Extend: vs = vs + <value>
    //----------------------------------------------------------------------------------------------
    {
      String update_template = "UPDATE " + tableName + " SET vs = vs - %s " +
          "WHERE h = %d AND r = %d";

      // Test removing existing and non-existing entries.
      session.execute(String.format(update_template, "{0, 2, 3}", 1, 1));
      // Checking row
      Row row = runSelect(String.format(select_template, 1, 1)).next();
      Set set = row.getSet("vs", Integer.class);
      assertEquals(1, set.size());
      assertTrue(set.contains(1));

      // Test removing null value from null column.
      session.execute(String.format(update_template, "{ }", 1, 2));
      // Checking row
      row = runSelect(String.format(select_template, 1, 2)).next();
      assertTrue(row.isNull("vs"));

      // Test removing from null column.
      session.execute(String.format(update_template, "{11, 22}", 1, 2));
      // Checking row
      row = runSelect(String.format(select_template, 1, 2)).next();
      set = row.getSet("vs", Integer.class);
      assertTrue(set.isEmpty());

      // Test removing a null value.
      session.execute(String.format(update_template, "{ }", 1, 1));
      // Checking row
      row = runSelect(String.format(select_template, 1, 1)).next();
      set = row.getSet("vs", Integer.class);
      assertEquals(1, set.size());
      assertTrue(set.contains(1));

      //------------------------------- Invalid Statements -----------------------------------------

      // Test wrong collection type.
      String invalidStmt =
          "UPDATE " + tableName + " SET vs = vs - [2, 3] WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test wrong elems type.
      invalidStmt = "UPDATE " + tableName + " SET vs = vs - {2.5} WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid argument: wrong order.
      invalidStmt = "UPDATE " + tableName + " SET vs = {'a', 'b'} - vs WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid arguments: two literals
      invalidStmt = "UPDATE " + tableName + " SET vl = {'a'} - {'v'} WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid arguments: two column refs
      invalidStmt = "UPDATE " + tableName + " SET vs = vs - vs WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);
    }

    //----------------------------------------------------------------------------------------------
    // Testing List Remove
    //----------------------------------------------------------------------------------------------
    {
      String update_template = "UPDATE " + tableName + " SET vl = vl - %s " +
          "WHERE h = %d AND r = %d";

      // Test remove existing ('x') and missing (z) elements.
      session.execute(String.format(update_template, "['x', 'z']", 1, 1));
      // Checking row (both entries of 'x' should be removed)
      Row row = runSelect(String.format(select_template, 1, 1)).next();
      List list = row.getList("vl", String.class);
      assertEquals(1, list.size());
      assertEquals("y", list.get(0));

      // Test removing null value from null column.
      session.execute(String.format(update_template, "[ ]", 1, 2));
      // Checking row
      row = runSelect(String.format(select_template, 1, 2)).next();
      assertTrue(row.isNull("vl"));

      // Test removing from null column.
      session.execute(String.format(update_template, "['bb', 'aa']", 1, 2));
      // Checking row
      row = runSelect(String.format(select_template, 1, 2)).next();
      assertTrue(row.isNull("vl"));

      // Test removing null value.
      session.execute(String.format(update_template, "[ ]", 1, 1));
      // Checking row
      row = runSelect(String.format(select_template, 1, 1)).next();
      list = row.getList("vl", String.class);
      assertEquals(1, list.size());
      assertEquals("y", list.get(0));

      //------------------------------- Invalid Statements -----------------------------------------

      // Test wrong collection type.
      String invalidStmt =
          "UPDATE " + tableName + " SET vl = vl - {'b', 'a'} WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test wrong elems type.
      invalidStmt = "UPDATE " + tableName + " SET vl = vl - [2.5] WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid arguments: wrong argument order
      invalidStmt = "UPDATE " + tableName + " SET vl = [2,3] - vl WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid arguments: two literals
      invalidStmt = "UPDATE " + tableName + " SET vl = [2] - [3] WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);

      // Test invalid arguments: two column refs
      invalidStmt = "UPDATE " + tableName + " SET vl = vl - vl WHERE h = 1 AND r = 1";
      runInvalidStmt(invalidStmt);
    }
  }

  @Test
  public void testCollectionIndex() throws Exception {

    //--------------------- Setting up for Insert and Update tests ---------------------------------
    String tableName = "test_coll_exp";

    String createStmt = createTableStmt(tableName, "int", "text");
    session.execute(createStmt);

    String insert_template = "INSERT INTO " + tableName +
        " (h, r, vm, vs, vl) VALUES (%d, %d, %s, %s, %s);";
    session.execute(String.format(insert_template, 1, 1, "{2 : 'b', 3: 'c'}",
        "{1, 2}", "['x', 'y']"));
    session.execute(String.format(insert_template, 1, 2, "{}", "{}", "[]"));

    String select_template = "SELECT * FROM " + tableName + " WHERE h = %d AND r = %d";

    //----------------------------------------------------------------------------------------------
    // Testing set clause for write operations
    //----------------------------------------------------------------------------------------------
    {
      String update_template = "UPDATE " + tableName + " SET %s WHERE h = %d AND r = %d";

      // -------------------------------- Testing Map ----------------------------------------------

      // Test updating existing value
      session.execute(String.format(update_template, "vm[2] = 'b1'", 1, 1));
      // Checking row
      Row row = runSelect(String.format(select_template, 1, 1)).next();
      Map map = row.getMap("vm", Integer.class, String.class);
      assertEquals(2, map.size());
      assertEquals("b1", map.get(2));
      assertEquals("c", map.get(3));

      // Test adding new value
      session.execute(String.format(update_template, "vm[1] = 'a'", 1, 1));
      // Checking row
      row = runSelect(String.format(select_template, 1, 1)).next();
      map = row.getMap("vm", Integer.class, String.class);
      assertEquals(3, map.size());
      assertEquals("a", map.get(1));
      assertEquals("b1", map.get(2));
      assertEquals("c", map.get(3));

      // Test deleting entry by setting value to null
      session.execute(String.format(update_template, "vm[2] = null", 1, 1));
      // Checking row
      row = runSelect(String.format(select_template, 1, 1)).next();
      map = row.getMap("vm", Integer.class, String.class);
      assertEquals(2, map.size());
      assertEquals("a", map.get(1));
      assertEquals("c", map.get(3));

      // Test adding value to null column
      session.execute(String.format(update_template, "vm[1] = 'a'", 1, 2));
      // Checking row
      row = runSelect(String.format(select_template, 1, 2)).next();
      map = row.getMap("vm", Integer.class, String.class);
      assertEquals(1, map.size());
      assertEquals("a", map.get(1));

      // Invalid stmt: wrong type (column type)
      runInvalidStmt(String.format(update_template, "vm[1] = {1 : 'a'}", 1, 2));

      // Invalid stmt: wrong type
      runInvalidStmt(String.format(update_template, "vm[1] = 2", 1, 2));

      // --------------------------------- Testing List --------------------------------------------

      // Test updating existing index
      session.execute(String.format(update_template, "vl[2] = 'y1'", 1, 1));
      // Checking row
      row = runSelect(String.format(select_template, 1, 1)).next();
      List list = row.getList("vl", String.class);
      assertEquals(2, list.size());
      assertEquals("x", list.get(0));
      assertEquals("y1", list.get(1));

      // Testing deleting elem by setting index to null
      session.execute(String.format(update_template, "vl[1] = null", 1, 1));
      // Checking row
      row = runSelect(String.format(select_template, 1, 1)).next();
      list = row.getList("vl", String.class);
      assertEquals(1, list.size());
      assertEquals("y1", list.get(0));

      // Invalid stmt: wrong type (column type)
      runInvalidStmt(String.format(update_template, "vm[1] = ['a']", 1, 2));

      // Invalid stmt: wrong type
      runInvalidStmt(String.format(update_template, "vm[1] = 2", 1, 2));

      // Invalid stmt: wrong index type
      runInvalidStmt(String.format(update_template, "vm[1.0] = 'a'", 1, 2));

      // Invalid stmt: index too large
      runInvalidStmt(String.format(update_template, "vl[3] = 'z'", 1, 1));

      // Invalid stmt: index too small
      runInvalidStmt(String.format(update_template, "vl[0] = 'a'", 1, 1));

      // Invalid stmt: null column (index always out of bounds)
      runInvalidStmt(String.format(update_template, "vl[1] = 'a'", 1, 2));

      // ---------------------------------- Testing Set --------------------------------------------

      // Invalid stmts: index-based access not allowed for set
      runInvalidStmt(String.format(update_template, "vs[1] = null", 1, 1));
      runInvalidStmt(String.format(update_template, "vs[1] = ''", 1, 1));
      runInvalidStmt(String.format(update_template, "vs[1] = 1", 1, 1));

    }

    //----------------------------------------------------------------------------------------------
    // Testing if clause (for write operations)
    //----------------------------------------------------------------------------------------------
    {
      // Setting up.
      session.execute(String.format(insert_template, 2, 1, "{2 : 'b', 3: 'c'}",
          "{1, 2}", "['x', 'y']"));
      session.execute(String.format(insert_template, 2, 2, "{}", "{}", "[]"));

      String update_template = "UPDATE " + tableName + " SET %s WHERE h = 2 AND r = %d IF %s";

      //-------------------------------- Testing Map -----------------------------------------

      // Test update map with true equality condition
      session.execute(String.format(update_template, "vm[2] = 'b1'", 1, "vm[2] = 'b'"));
      // Checking row -- should apply.
      Row row = runSelect(String.format(select_template, 2, 1)).next();
      Map map = row.getMap("vm", Integer.class, String.class);
      assertEquals(2, map.size());
      assertEquals("b1", map.get(2));
      assertEquals("c", map.get(3));

      // Test update map with false equality condition
      session.execute(String.format(update_template, "vm[2] = 'b2'", 1, "vm[2] = 'b'"));
      // Checking row -- should do nothing.
      row = runSelect(String.format(select_template, 2, 1)).next();
      map = row.getMap("vm", Integer.class, String.class);
      assertEquals(2, map.size());
      assertEquals("b1", map.get(2));
      assertEquals("c", map.get(3));

      // Testing comparing (equality with) null
      session.execute(String.format(update_template, "vm[22] = 'x'", 1, "vm[22] = null"));
      // Checking row -- should add element.
      row = runSelect(String.format(select_template, 2, 1)).next();
      map = row.getMap("vm", Integer.class, String.class);
      assertEquals(3, map.size());
      assertEquals("b1", map.get(2));
      assertEquals("c", map.get(3));
      assertEquals("x", map.get(22));

      // Test update map with true inequality condition
      session.execute(String.format(update_template, "vm[2] = 'b2'", 1, "vm[2] > 'b0'"));
      // Checking row -- should apply.
      row = runSelect(String.format(select_template, 2, 1)).next();
      map = row.getMap("vm", Integer.class, String.class);
      assertEquals(3, map.size());
      assertEquals("b2", map.get(2));
      assertEquals("c", map.get(3));
      assertEquals("x", map.get(22));

      // Test update map with false inequality condition
      session.execute(String.format(update_template, "vm[2] = 'b3'", 1, "vm[2] <= 'b1'"));
      // Checking row -- should do nothing.
      row = runSelect(String.format(select_template, 2, 1)).next();
      map = row.getMap("vm", Integer.class, String.class);
      assertEquals(3, map.size());
      assertEquals("b2", map.get(2));
      assertEquals("c", map.get(3));
      assertEquals("x", map.get(22));

      // Invalid Stmt: only equality comparison is allowed with null
      runInvalidStmt(String.format(update_template, "vm[2] = 'b2'", 1, "vm[2] > null"));
      runInvalidStmt(String.format(update_template, "vm[2] = 'b2'", 1, "vm[2] >= null"));
      runInvalidStmt(String.format(update_template, "vm[2] = 'b2'", 1, "vm[2] < null"));
      runInvalidStmt(String.format(update_template, "vm[2] = 'b2'", 1, "vm[2] <= null"));

      //-------------------------------- Testing Set -----------------------------------------------

      // Invalid Stmt: set indexing not allowed
      runInvalidStmt(String.format(update_template, "vm[2] = 'b2'", 1, "vs[3] = ''"));

      //-------------------------------- Testing List ----------------------------------------------

      // Test update list with true equality condition
      session.execute(String.format(update_template, "vl[2] = 'y1'", 1, "vl[2] = 'y'"));
      // Checking row -- should apply.
      row = runSelect(String.format(select_template, 2, 1)).next();
      List list = row.getList("vl", String.class);
      assertEquals(2, list.size());
      assertEquals("x", list.get(0));
      assertEquals("y1", list.get(1));

      // Test update list with false equality condition
      session.execute(String.format(update_template, "vl[2] = 'y2'", 1, "vl[2] = 'y'"));
      // Checking row -- should do nothing.
      row = runSelect(String.format(select_template, 2, 1)).next();
      list = row.getList("vl", String.class);
      assertEquals(2, list.size());
      assertEquals("x", list.get(0));
      assertEquals("y1", list.get(1));

      // Test update list with true inequality condition
      session.execute(String.format(update_template, "vl[2] = 'y2'", 1, "vl[2] >= 'y1'"));
      // Checking row -- should do nothing.
      row = runSelect(String.format(select_template, 2, 1)).next();
      list = row.getList("vl", String.class);
      assertEquals(2, list.size());
      assertEquals("x", list.get(0));
      assertEquals("y2", list.get(1));

      // Test update list with true inequality condition
      session.execute(String.format(update_template, "vl[2] = 'y3'", 1, "vl[2] < 'y2'"));
      // Checking row -- should do nothing.
      row = runSelect(String.format(select_template, 2, 1)).next();
      list = row.getList("vl", String.class);
      assertEquals(2, list.size());
      assertEquals("x", list.get(0));
      assertEquals("y2", list.get(1));

      // Invalid Stmt: compare with null
      runInvalidStmt(String.format(update_template, "vl[2] = 'y2'", 1, "vl[2] > null"));
      runInvalidStmt(String.format(update_template, "vl[2] = 'y2'", 1, "vl[2] <= null"));
    }

    //----------------------------------------------------------------------------------------------
    // Testing where clause (for read operations)
    //----------------------------------------------------------------------------------------------
    {
      // Setting up.
      session.execute(String.format(insert_template, 3, 1, "{2 : 'b', 3: 'c'}",
          "{1, 2}", "['x', 'y']"));
      session.execute(String.format(insert_template, 2, 2, "{}", "{}", "[]"));
      select_template = "SELECT * FROM " + tableName + " WHERE h = 3 AND r = %d AND %s";

      // Test select with true equality condition
      Iterator<Row> it = runSelect(String.format(select_template, 1, "vm[2] = 'b'"));
      // expecting one row
      Row row = it.next();
      assertFalse(it.hasNext());
      // Checking row
      Map map = row.getMap("vm", Integer.class, String.class);
      assertEquals(2, map.size());
      assertEquals("b", map.get(2));
      assertEquals("c", map.get(3));

      // Test select with false equality condition
      it = session.execute(String.format(select_template, 1, "vl[2] = 'y1'")).iterator();
      // expecting no rows
      assertFalse(it.hasNext());

      // Test select with true inequality condition
      it = runSelect(String.format(select_template, 1, "vl[1] <= 'z'"));
      // expecting one row
      row = it.next();
      assertFalse(it.hasNext());
      // Checking row
      List list = row.getList("vl", String.class);
      assertEquals(2, list.size());
      assertEquals("x", list.get(0));
      assertEquals("y", list.get(1));

      // Test select with false inequality condition
      it = session.execute(String.format(select_template, 1, "vm[3] > 'c'")).iterator();
      // expecting no rows
      assertFalse(it.hasNext());

      // Invalid Stmt: wrong type.
      runInvalidStmt(String.format(select_template, 1, "vm[2] = 3"));
      runInvalidStmt(String.format(select_template, 1, "vm[2] = {3 : 'a'}"));
      runInvalidStmt(String.format(select_template, 1, "vl[2] = 3"));
      runInvalidStmt(String.format(select_template, 1, "vl[1] = ['a']"));

      // Invalid Stmt: set indexing not allowed.
      runInvalidStmt(String.format(select_template, 1, "vs[2] = ''"));
    }
  }

  @Test
  public void testCollectionExpressionsWithStaticColumns() throws Exception {

    //--------------------- Setting up for Insert and Update tests ---------------------------------
    String tableName = "test_coll_exp";

    String createTableStmt = "CREATE TABLE " + tableName + " (h int, r int, " +
        "vm map<int, text> static, vs set<int> static, vl list<text> static, " +
        "primary key((h), r));";
    session.execute(createTableStmt);
    String insert_template = "INSERT INTO " + tableName +
        " (h, r, vm, vs, vl) VALUES (%d, %d, %s, %s, %s);";
    session.execute(String.format(insert_template, 1, 1, "{}", "{}", "[]"));
    session.execute(String.format(insert_template, 2, 1, "{2 : 'b', 3: 'c'}",
        "{1, 2}", "['x', 'y']"));
    String select_template = "SELECT * FROM " + tableName + " WHERE h = %d AND r = %d";

    //----------------------------------------------------------------------------------------------
    // Testing insert (missing range key should be allowed).
    //----------------------------------------------------------------------------------------------
    {
      // Setting up.
      Map<Integer, String> map = new HashMap<>();
      map.put(2, "b");
      map.put(3, "c");
      Set<Integer> set = new HashSet<>();
      set.add(1);
      set.add(2);
      List<String> list = new ArrayList<>();
      list.add("x");
      list.add("y");

      // Checking initial state.
      Row row = runSelect(String.format(select_template, 1, 1)).next();
      assertTrue(row.isNull("vm"));
      assertTrue(row.isNull("vs"));
      assertTrue(row.isNull("vl"));

      row = runSelect(String.format(select_template, 2, 1)).next();
      assertEquals(map, row.getMap("vm", Integer.class, String.class));
      assertEquals(set, row.getSet("vs", Integer.class));
      assertEquals(list, row.getList("vl", String.class));


      // Test insert with no range columns (flip values for the two hash keys).
      String insert_no_range = "INSERT INTO " + tableName +
          " (h, vm, vs, vl) VALUES (%d, %s, %s, %s);";
      session.execute(String.format(insert_no_range, 1, "{2 : 'b', 3: 'c'}",
          "{1, 2}", "['x', 'y']"));
      session.execute(String.format(insert_no_range, 2, "{}", "{}", "[]"));

      // Checking rows.
      row = runSelect(String.format(select_template, 1, 1)).next();
      assertEquals(map, row.getMap("vm", Integer.class, String.class));
      assertEquals(set, row.getSet("vs", Integer.class));
      assertEquals(list, row.getList("vl", String.class));

      row = runSelect(String.format(select_template, 2, 1)).next();
      assertTrue(row.isNull("vm"));
      assertTrue(row.isNull("vs"));
      assertTrue(row.isNull("vl"));
    }
    //----------------------------------------------------------------------------------------------
    // Testing update (missing range key should be allowed).
    //----------------------------------------------------------------------------------------------
    {
      String update_template = "UPDATE " + tableName + " SET %s WHERE h = %d";

      // -------------------------------- Testing Map ----------------------------------------------

      // Test updating existing value
      session.execute(String.format(update_template, "vm[2] = 'b1'", 1, 1));
      // Checking row
      Row row = runSelect(String.format(select_template, 1, 1)).next();
      Map map = row.getMap("vm", Integer.class, String.class);
      assertEquals(2, map.size());
      assertEquals("b1", map.get(2));
      assertEquals("c", map.get(3));

      // Test adding new value
      session.execute(String.format(update_template, "vm[1] = 'a'", 1, 1));
      // Checking row
      row = runSelect(String.format(select_template, 1, 1)).next();
      map = row.getMap("vm", Integer.class, String.class);
      assertEquals(3, map.size());
      assertEquals("a", map.get(1));
      assertEquals("b1", map.get(2));
      assertEquals("c", map.get(3));

      // Test deleting entry by setting value to null
      session.execute(String.format(update_template, "vm[2] = null", 1, 1));
      // Checking row
      row = runSelect(String.format(select_template, 1, 1)).next();
      map = row.getMap("vm", Integer.class, String.class);
      assertEquals(2, map.size());
      assertEquals("a", map.get(1));
      assertEquals("c", map.get(3));

      // Test adding value to null column
      session.execute(String.format(update_template, "vm[1] = 'a'", 2, 1));
      // Checking row
      row = runSelect(String.format(select_template, 2, 1)).next();
      map = row.getMap("vm", Integer.class, String.class);
      assertEquals(1, map.size());
      assertEquals("a", map.get(1));

      // Invalid stmt: wrong type (column type)
      runInvalidStmt(String.format(update_template, "vm[1] = {1 : 'a'}", 2, 1));

      // Invalid stmt: wrong type
      runInvalidStmt(String.format(update_template, "vm[1] = 2", 2, 1));

      // --------------------------------- Testing List --------------------------------------------

      // Test updating existing index
      session.execute(String.format(update_template, "vl[2] = 'y1'", 1, 1));
      // Checking row
      row = runSelect(String.format(select_template, 1, 1)).next();
      List list = row.getList("vl", String.class);
      assertEquals(2, list.size());
      assertEquals("x", list.get(0));
      assertEquals("y1", list.get(1));

      // Testing deleting elem by setting index to null
      session.execute(String.format(update_template, "vl[1] = null", 1, 1));
      // Checking row
      row = runSelect(String.format(select_template, 1, 1)).next();
      list = row.getList("vl", String.class);
      assertEquals(1, list.size());
      assertEquals("y1", list.get(0));

      // Invalid stmt: wrong type (column type)
      runInvalidStmt(String.format(update_template, "vm[1] = ['a']", 2, 1));

      // Invalid stmt: wrong type
      runInvalidStmt(String.format(update_template, "vm[1] = 2", 2, 1));

      // Invalid stmt: index too large
      runInvalidStmt(String.format(update_template, "vl[3] = 'z'", 1, 1));

      // Invalid stmt: index too small
      runInvalidStmt(String.format(update_template, "vl[0] = 'a'", 1, 1));

      // Invalid stmt: null column (index always out of bounds)
      runInvalidStmt(String.format(update_template, "vl[0] = 'a'", 2, 1));

      // ---------------------------------- Testing Set --------------------------------------------

      // Invalid stmts: index-based access not allowed for set
      runInvalidStmt(String.format(update_template, "vs[1] = null", 1, 1));
      runInvalidStmt(String.format(update_template, "vs[1] = ''", 1, 1));
      runInvalidStmt(String.format(update_template, "vs[1] = 1", 1, 1));
    }
  }

  @Test
  public void testReplaceInListWithTtl() throws Exception {

    //-------------------------------------- Setting up --------------------------------------------
    String tableName = "test_coll_exp";

    String createTableStmt = "CREATE TABLE " + tableName + " (h int, r int, " +
        "vm map<int, text> static, vs set<int> static, vl list<text> static, " +
        "primary key((h), r));";
    session.execute(createTableStmt);
    String insert_template = "INSERT INTO " + tableName +
        " (h, r, vm, vs, vl) VALUES (%d, %d, %s, %s, %s);";
    session.execute(String.format(insert_template, 1, 1, "{}", "{}", "['a', 'b', 'c']"));

    String select_template = "SELECT * FROM " + tableName + " WHERE h = %d AND r = %d";
    String update_template =
        "UPDATE " + tableName + " USING TTL %d SET %s WHERE h = %d AND r = %d";

    //----------------------------------------------------------------------------------------------
    // Testing update list index with TTL
    // Note: Replacing in list requires reading entries to find right index in addition to
    // updating the value -- this checks that read-time and entry TTL interact as expected
    //----------------------------------------------------------------------------------------------
    session.execute(String.format(update_template, 1, "vl[2] = 'b1'", 1, 1));
    Row row = runSelect(String.format(select_template, 1, 1)).next();
    List list = row.getList("vl", String.class);
    assertEquals(3, list.size());
    assertEquals("a", list.get(0));
    assertEquals("b1", list.get(1));
    assertEquals("c", list.get(2));
    Thread.sleep(1050);

    // Check entry expired
    row = runSelect(String.format(select_template, 1, 1)).next();
    list = row.getList("vl", String.class);
    assertEquals(2, list.size());
    assertEquals("a", list.get(0));
    assertEquals("c", list.get(1));

    // This should skip the expired element
    session.execute(String.format(update_template, 1, "vl[2] = 'c1'", 1, 1));
    row = runSelect(String.format(select_template, 1, 1)).next();
    list = row.getList("vl", String.class);
    assertEquals(2, list.size());
    assertEquals("a", list.get(0));
    assertEquals("c1", list.get(1));
    Thread.sleep(1050);

    // Check entry expired
    row = runSelect(String.format(select_template, 1, 1)).next();
    list = row.getList("vl", String.class);
    assertEquals(1, list.size());
    assertEquals("a", list.get(0));
  }

  @Test
  public void testCollectionExpressionsWithBind() throws Exception {
    //------------------------------------- Setting up ---------------------------------------------
    String tableName = "test_bind_coll_exp";

    String createStmt = createTableStmt(tableName, "int", "text");
    session.execute(createStmt);

    String insert_template = "INSERT INTO " + tableName +
        " (h, r, vm, vs, vl) VALUES (%d, %d, %s, %s, %s);";
    session.execute(String.format(insert_template, 1, 1, "{2 : 'b', 3: 'c'}",
        "{1, 2}", "['x', 'y']"));
    session.execute(String.format(insert_template, 1, 2, "{}", "{}", "[]"));

    String select_template = "SELECT * FROM " + tableName + " WHERE h = %d AND r = %d";

    //----------------------------------------------------------------------------------------------
    // Testing Map
    //----------------------------------------------------------------------------------------------
    {
      //---------------------------- Extend: vm = vm + <value> -------------------------------------

      Map<Integer, String> map = new HashMap<>();
      map.put(1, "a");
      map.put(3, "c1");
      session.execute("UPDATE " + tableName + " SET vm = vm + ? WHERE h = ? AND r = ?",
          map, new Integer(1), new Integer(1));

      // Checking row -- expecting key 1 is added (val "a"), key 3 is overwritten (to "c1")
      Row row = runSelect(String.format(select_template, 1, 1)).next();
      map = row.getMap("vm", Integer.class, String.class);
      assertEquals(3, map.size());
      assertEquals("a", map.get(1));
      assertEquals("b", map.get(2));
      assertEquals("c1", map.get(3));

      //--------------------------- Subtract: vm = vm - <value> ------------------------------------

      Set<Integer> set = new HashSet<>();
      set.add(1);
      set.add(3);
      session.execute("UPDATE " + tableName + " SET vm = vm - ? WHERE h = ? AND r = ?",
          set, new Integer(1), new Integer(1));

      // Checking row -- expecting entries at keys 1 and 3 are removed.
      row = runSelect(String.format(select_template, 1, 1)).next();
      map = row.getMap("vm", Integer.class, String.class);
      assertEquals(1, map.size());
      assertEquals("b", map.get(2));
    }


    //----------------------------------------------------------------------------------------------
    // Testing Set
    //----------------------------------------------------------------------------------------------
    {
      //---------------------------- Extend: vs = vs + <value> -------------------------------------

      Set<Integer> set = new HashSet<>();
      set.add(0);
      set.add(2);
      set.add(3);
      session.execute("UPDATE " + tableName + " SET vs = vs + ? WHERE h = ? AND r = ?",
          set, new Integer(1), new Integer(1));

      // Checking row, expecting result is set union.
      Row row = runSelect(String.format(select_template, 1, 1)).next();
      set = row.getSet("vs", Integer.class);
      assertEquals(4, set.size());
      assertTrue(set.contains(0));
      assertTrue(set.contains(1));
      assertTrue(set.contains(2));
      assertTrue(set.contains(3));

      //--------------------------- Subtract: vs = vs - <value> ------------------------------------

      set.clear();
      set.add(1);
      set.add(3);
      session.execute("UPDATE " + tableName + " SET vs = vs - ? WHERE h = ? AND r = ?",
          set, new Integer(1), new Integer(1));

      // Checking row -- expecting elems 1 and 3 are removed.
      row = runSelect(String.format(select_template, 1, 1)).next();
      set = row.getSet("vs", Integer.class);
      assertEquals(2, set.size());
      assertTrue(set.contains(0));
      assertTrue(set.contains(2));
    }

    //----------------------------------------------------------------------------------------------
    // Testing List
    //----------------------------------------------------------------------------------------------
    {
      //---------------------------- Append: vl = vl + <value> -------------------------------------

      List<String> list = new ArrayList<>();
      list.add("v");
      list.add("w");
      session.execute("UPDATE " + tableName + " SET vl = vl + ? WHERE h = ? AND r = ?",
          list, new Integer(1), new Integer(1));

      // Checking row, expecting 'v', 'w' to be added at the end.
      Row row = runSelect(String.format(select_template, 1, 1)).next();
      list = row.getList("vl", String.class);
      assertEquals(4, list.size());
      assertEquals("x", list.get(0));
      assertEquals("y", list.get(1));
      assertEquals("v", list.get(2));
      assertEquals("w", list.get(3));

      //---------------------------- Prepend: vl = <value> + vl ------------------------------------

      list.clear();
      list.add("z");

      session.execute("UPDATE " + tableName + " SET vl = ? + vl WHERE h = ? AND r = ?",
          list, new Integer(1), new Integer(1));

      // Checking row, expecting 'z' to be added at the beginning.
      row = runSelect(String.format(select_template, 1, 1)).next();
      list = row.getList("vl", String.class);
      assertEquals(5, list.size());
      assertEquals("z", list.get(0));
      assertEquals("x", list.get(1));
      assertEquals("y", list.get(2));
      assertEquals("v", list.get(3));
      assertEquals("w", list.get(4));

      //---------------------------- Remove: vl = vl - <value> -------------------------------------

      list.clear();
      list.add("x");
      list.add("v");

      session.execute("UPDATE " + tableName + " SET vl = vl - ? WHERE h = ? AND r = ?",
          list, new Integer(1), new Integer(1));

      // Checking row, expecting 'x' and 'v' values to be removed.
      row = runSelect(String.format(select_template, 1, 1)).next();
      list = row.getList("vl", String.class);
      assertEquals(3, list.size());
      assertEquals("z", list.get(0));
      assertEquals("y", list.get(1));
      assertEquals("w", list.get(2));
    }
  }
}
