// Q2. Tag evolution
// Kuzu adaptation: Message is represented by the POST:COMMENT union.
// Kuzu 0.10 needs the parameter-derived window boundaries materialized with explicit TIMESTAMP casts.
// Query edit (IncGQ middleware): the three window boundaries are written into the
// predicates instead of being bound by a leading WITH. They are deterministic
// expressions of $date and the leading WITH yields exactly one row, so substituting
// them changes nothing; with $date specialized they fold to the TIMESTAMP literals
// the parameter-dependent FILLs use (cf. Kuzu/queries/optimized/ldbc-bi/bi-2.cypher).
MATCH (tag:TAG)-[:HASTYPE]->(:TAGCLASS {name: $tagClass})
OPTIONAL MATCH (message1:POST:COMMENT)-[:HASTAG]->(tag)
  WHERE CAST($date, 'TIMESTAMP') <= message1.creationDate
    AND message1.creationDate < CAST($date, 'TIMESTAMP') + duration('100 days')
WITH tag, count(message1) AS countWindow1
OPTIONAL MATCH (message2:POST:COMMENT)-[:HASTAG]->(tag)
  WHERE CAST($date, 'TIMESTAMP') + duration('100 days') <= message2.creationDate
    AND message2.creationDate < CAST($date, 'TIMESTAMP') + duration('200 days')
WITH tag, countWindow1, count(message2) AS countWindow2
RETURN
  tag.name AS name,
  countWindow1,
  countWindow2,
  abs(countWindow1 - countWindow2) AS diff
ORDER BY diff DESC, name ASC
LIMIT 100
