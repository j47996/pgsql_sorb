--
-- JGH Sorb performance
--
set optimize_unique_node to on;

CREATE TABLE jgh (i integer);
INSERT INTO jgh (i) SELECT 20000 * random() FROM generate_series(1, 20000);
VACUUM ANALYZE jgh;

-- external sort
explain analyze SELECT * FROM jgh ORDER BY i;

set enable_hashagg to off;
set work_mem to 16384;
-- internal merge
explain analyze SELECT * FROM jgh ORDER BY i;
set enable_intmerge_sort to off;
-- internal quicksort
explain analyze SELECT * FROM jgh ORDER BY i;
set enable_intmerge_sort to on;
-- internal merge retry
explain analyze SELECT * FROM jgh ORDER BY i;
set enable_intmerge_sort to off;
-- internal quicksort retry
explain analyze SELECT * FROM jgh ORDER BY i;
set enable_intmerge_sort to on;


explain analyze SELECT distinct i FROM jgh;
set enable_hashagg to off;
explain analyze SELECT distinct i FROM jgh;
set optimize_unique_node to off;
explain analyze SELECT distinct i FROM jgh;
set optimize_dedup_sort to off;
explain analyze SELECT distinct i FROM jgh;
set enable_intmerge_sort to off;
explain analyze SELECT distinct i FROM jgh;
DROP TABLE jgh;

----------------------------------------
create table jgh (s text, i integer);
INSERT INTO jgh (s, i) SELECT concat(
		chr((100*random()+20)::int),
		chr((100*random()+20)::int)),
20000 * random() FROM generate_series(1, 20000);
VACUUM ANALYZE jgh;

set work_mem to 1024;
set enable_hashagg to on;
set optimize_dedup_sort to on;
set enable_intmerge_sort to on;
set optimize_unique_node to on;

-- external sort
explain analyze SELECT * FROM jgh ORDER BY s, i;

set enable_hashagg to off;
set work_mem to 16384;
-- internal merge
explain analyze SELECT * FROM jgh ORDER BY s, i;
set enable_intmerge_sort to off;
-- internal quicksort
explain analyze SELECT * FROM jgh ORDER BY s, i;
set enable_intmerge_sort to on;
-- internal merge retry
explain analyze SELECT * FROM jgh ORDER BY s, i;
set enable_intmerge_sort to off;
-- internal quicksort retry
explain analyze SELECT * FROM jgh ORDER BY s, i;
set enable_intmerge_sort to on;


explain analyze SELECT distinct s FROM jgh;
set enable_hashagg to off;
explain analyze SELECT distinct s FROM jgh;
set optimize_unique_node to off;
explain analyze SELECT distinct s FROM jgh;
set optimize_dedup_sort to off;
explain analyze SELECT distinct s FROM jgh;
set enable_intmerge_sort to off;
explain analyze SELECT distinct s FROM jgh;
DROP TABLE jgh;

----------------------------------------
-- larger table
CREATE TABLE jgh (i integer);
INSERT INTO jgh (i) SELECT 200000 * random() FROM generate_series(1, 200000);
VACUUM ANALYZE jgh;

set work_mem to 1024;
set enable_hashagg to on;
set optimize_dedup_sort to on;
set enable_intmerge_sort to on;
set optimize_unique_node to on;

-- external sort
explain analyze SELECT * FROM jgh ORDER BY i;

set enable_hashagg to off;
set work_mem to 163840;
-- internal merge
explain analyze SELECT * FROM jgh ORDER BY i;
set enable_intmerge_sort to off;
-- internal quicksort
explain analyze SELECT * FROM jgh ORDER BY i;
set enable_intmerge_sort to on;
-- internal merge retry
explain analyze SELECT * FROM jgh ORDER BY i;
set enable_intmerge_sort to off;
-- internal quicksort retry
explain analyze SELECT * FROM jgh ORDER BY i;
set enable_intmerge_sort to on;


explain analyze SELECT distinct i FROM jgh;
set enable_hashagg to off;
explain analyze SELECT distinct i FROM jgh;
set optimize_unique_node to off;
explain analyze SELECT distinct i FROM jgh;
set optimize_dedup_sort to off;
explain analyze SELECT distinct i FROM jgh;
set enable_intmerge_sort to off;
explain analyze SELECT distinct i FROM jgh;
DROP TABLE jgh;

----------------------------------------
create table jgh (s text, i integer);
INSERT INTO jgh (s, i) SELECT concat(
		chr((100*random()+20)::int),
		chr((100*random()+20)::int),
		chr((100*random()+20)::int),
		chr((100*random()+20)::int),
		chr((100*random()+20)::int)),
200000 * random() FROM generate_series(1, 200000);
VACUUM ANALYZE jgh;

set work_mem to 1024;
set enable_hashagg to on;
set optimize_dedup_sort to on;
set enable_intmerge_sort to on;
set optimize_unique_node to on;

-- external sort
explain analyze SELECT * FROM jgh ORDER BY s, i;

set enable_hashagg to off;
set work_mem to 163840;
-- internal merge
explain analyze SELECT * FROM jgh ORDER BY s, i;
set enable_intmerge_sort to off;
-- internal quicksort
explain analyze SELECT * FROM jgh ORDER BY s, i;
set enable_intmerge_sort to on;
-- internal merge retry
explain analyze SELECT * FROM jgh ORDER BY s, i;
set enable_intmerge_sort to off;
-- internal quicksort retry
explain analyze SELECT * FROM jgh ORDER BY s, i;
set enable_intmerge_sort to on;


explain analyze SELECT distinct s FROM jgh;
set enable_hashagg to off;
explain analyze SELECT distinct s FROM jgh;
set optimize_unique_node to off;
explain analyze SELECT distinct s FROM jgh;
set optimize_dedup_sort to off;
explain analyze SELECT distinct s FROM jgh;
set enable_intmerge_sort to off;
explain analyze SELECT distinct s FROM jgh;
DROP TABLE jgh;

----------------------------------------

set work_mem to 1024;
set enable_hashagg to on;
set optimize_dedup_sort to on;
set enable_intmerge_sort to on;
set optimize_unique_node to on;

CREATE TABLE jgh (i integer);
INSERT INTO jgh (i) SELECT * FROM generate_series(1, 10);
INSERT INTO jgh (i) SELECT * FROM generate_series(1, 10);
INSERT INTO jgh (i) SELECT * FROM generate_series(1, 10000);
INSERT INTO jgh (i) SELECT * FROM generate_series(1, 10000);
VACUUM ANALYZE jgh;

explain analyze SELECT DISTINCT i FROM jgh ORDER BY i;

set enable_hashagg to off;
explain analyze SELECT DISTINCT i FROM jgh ORDER BY i;

set optimize_unique_node to off;
explain analyze SELECT DISTINCT i FROM jgh ORDER BY i;

set optimize_dedup_sort to off;
explain analyze SELECT DISTINCT i FROM jgh ORDER BY i;

set enable_intmerge_sort to off;
explain analyze SELECT DISTINCT i FROM jgh ORDER BY i;

set work_mem to 16384;

explain analyze SELECT DISTINCT i FROM jgh ORDER BY i;

set enable_intmerge_sort to on;
explain analyze SELECT DISTINCT i FROM jgh ORDER BY i;

set optimize_dedup_sort to on;;
explain analyze SELECT DISTINCT i FROM jgh ORDER BY i;

set optimize_unique_node to on;
explain analyze SELECT DISTINCT i FROM jgh ORDER BY i;

set enable_hashagg to on;
explain analyze SELECT DISTINCT i FROM jgh ORDER BY i;


explain analyze SELECT DISTINCT i FROM jgh ORDER BY i LIMIT 10;

set enable_hashagg to off;
explain analyze SELECT DISTINCT i FROM jgh ORDER BY i LIMIT 10;

set optimize_unique_node to off;
explain analyze SELECT DISTINCT i FROM jgh ORDER BY i LIMIT 10;

set optimize_dedup_sort to off;
explain analyze SELECT DISTINCT i FROM jgh ORDER BY i LIMIT 10;

set enable_intmerge_sort to off;
explain analyze SELECT DISTINCT i FROM jgh ORDER BY i LIMIT 10;



DROP TABLE jgh;

