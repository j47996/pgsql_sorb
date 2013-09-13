--
-- JGH Bughunting
--

CREATE TABLE jgh (i integer);
INSERT INTO jgh (i) SELECT * FROM generate_series(1, 10);
INSERT INTO jgh (i) SELECT * FROM generate_series(1, 10);
INSERT INTO jgh (i) SELECT * FROM generate_series(1, 10000);
INSERT INTO jgh (i) SELECT * FROM generate_series(1, 10000);
set enable_hashagg to off;
explain analyze SELECT DISTINCT i FROM jgh ORDER BY i;
DROP TABLE jgh;

