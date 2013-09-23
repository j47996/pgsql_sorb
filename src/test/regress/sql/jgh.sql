--
-- JGH Bughunting
--

CREATE TABLE jgh (i integer);
INSERT INTO jgh (i) SELECT 20000 * random() FROM generate_series(1, 20000);

explain analyze SELECT * FROM jgh ORDER BY i;

set work_mem to 16384;
explain analyze SELECT * FROM jgh ORDER BY i;

explain analyze SELECT distinct i FROM jgh;
set enable_hashagg to off;
explain analyze SELECT distinct i FROM jgh;
set optimize_dedup_sort to off;
explain analyze SELECT distinct i FROM jgh;
set enable_intmerge_sort to off;
explain analyze SELECT distinct i FROM jgh;
DROP TABLE jgh;

set work_mem to 1024;
set enable_hashagg to on;
set optimize_dedup_sort to on;
set enable_intmerge_sort to on;

CREATE TABLE jgh (i integer);
INSERT INTO jgh (i) SELECT * FROM generate_series(1, 10);
INSERT INTO jgh (i) SELECT * FROM generate_series(1, 10);
INSERT INTO jgh (i) SELECT * FROM generate_series(1, 10000);
INSERT INTO jgh (i) SELECT * FROM generate_series(1, 10000);

explain analyze SELECT DISTINCT i FROM jgh ORDER BY i;

set enable_hashagg to off;
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

set enable_hashagg to on;
explain analyze SELECT DISTINCT i FROM jgh ORDER BY i;


explain analyze SELECT DISTINCT i FROM jgh ORDER BY i LIMIT 10;
set enable_hashagg to off;
explain analyze SELECT DISTINCT i FROM jgh ORDER BY i LIMIT 10;

DROP TABLE jgh;

