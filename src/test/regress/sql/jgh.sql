--
-- JGH Bughunting
--

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

DROP TABLE jgh;

