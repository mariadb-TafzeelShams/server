--echo # Table-level hint
explain select /*+ no_bnl(t2@qb__dt)*/ * from
  (select t1.* from t1, t2 where t1.a > t2.a) as dt;

--echo # QB-level hint
explain select /*+ no_bnl(@qb__dt)*/ * from
  (select t1.* from t1, t2 where t1.a > t2.a) as dt;

--echo # Index-level hints
--echo # Without the hint 'range' index access would be chosen
explain select /*+ no_index(t1@`qb__T`)*/ * from
  (select * from t1 where a < 3) T;

--echo # Without the hint 'range' index access would be chosen
explain select /*+ no_range_optimization(t1@qb__t1)*/ *  from
  (select * from t1 where a > 100 and a < 120) as t1;

--echo # Regular and derived tables share same name but the hint is applied correctly
explain select /*+ index(t1@qb__t1 idx_ab)*/ * from
  (select * from t1 where a < 3) as T1;

explain select /*+ no_index(t1@qb__t2 idx_a) index(t1@qb__t1 idx_ab)*/ * from
  (select * from t1 where a < 3) as T1, (select * from t1 where a < 5) as T2;

explain select /*+ no_index(t1@qb__t1 idx_a, idx_ab)*/ * from
  (select * from t1 where a < 3) as T1, (select * from t1 where a < 5) as T2;

--echo # Nested derived tables
explain select /*+ no_bnl(t1@qb__dt2)*/ * from
  (select count(*) from t1, (select * from t1 where a < 5) DT1) as DT2;

explain select /*+ no_index(t1@qb__DT2)*/ * from
  (select count(*) from t1, (select * from t1 where a < 5) DT1) as DT2;

--echo # Both hints are applied
explain select /*+ no_index(t1@qb__dt1) no_bnl(t1@qb__dt2)*/ * from
  (select count(*) from t1, (select * from t1 where a < 5) DT1) as DT2;

--echo # Nested derived tables with ambiguous names
explain select * from
  (select count(*) from t1, (select * from t1 where a < 5) T1) as T1;

--echo # Warning on ambiguous query block name, hint is ignored
explain select /*+ no_index(t1@qb__t1)*/* from
  (select count(*) from t1, (select * from t1 where a < 5) T1) as T1;

--echo # The hint cannot be applied to a derived table with UNION
explain select /*+ no_index(t2@qb__t1)*/* from
  (select * from t1 where a < 3 union select * from t2 where a < 9) as T1;

explain select /*+ no_index(t1@qb__t1)*/* from
  (select * from t1 where a < 3 union select * from t2 where a < 9) as T1;

--echo # ======================================
--echo # Test CTEs
--echo # By default BNL and index access to t1 is used.
explain
with cte as (select count(*) from t1, (select * from t1 where a < 5) DT1)
select * from cte;

explain
with cte as (select count(*) from t1, (select * from t1 where a < 5) DT1)
select /*+ no_bnl(t1@qb__cte)*/ * from cte;

explain
with cte as (select count(*) from t1, (select * from t1 where a < 5) DT1)
select /*+ no_bnl(@qb__cte)*/ * from cte;

explain
with cte as (select count(*) from t1, (select * from t1 where a < 5) DT1)
select /*+ no_index(t1@qb__dt1)*/ * from cte;

explain
with cte as (select count(*) from t1, (select * from t1 where a < 5) DT1)
select /*+ no_index(t1@qb__dt1 idx_a) no_bnl(@qb__cte)*/ * from cte;

--echo # ======================================
--echo # Test views
create view v1 as select * from t1 where a < 100;

--echo # Default execution plan
explain select *
  from v1, v1 as v2 where v1.a = v2.a and v1.a < 3;

explain select /*+ index(t1@`qb__v1` idx_ab) no_index(t1@`qb__v2`)*/ *
  from v1, v1 as v2 where v1.a = v2.a and v1.a < 3;

--echo # Nested views
create view v3 as select * from v1 where a < 300;

--echo # Default execution plan
explain select * from v3;

--echo # Addressing an object inside a nested view
explain select /*+ index(t1@`qb__v1` idx_ab)*/ * from v3;

drop view v1, v3;