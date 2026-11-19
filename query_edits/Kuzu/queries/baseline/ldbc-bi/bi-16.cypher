// Q16. Fake news detection
// Kuzu adaptation: replaces Neo4j CALL subquery with explicit A/B branches in a single pipeline.
// Query edit (IncGQ middleware), two steps that keep the result:
// 1. The day test CAST(CAST(m.creationDate AS TIMESTAMP) AS DATE) = CAST($dateX AS DATE)
//    is written as the half-open interval [date_trunc('day', $dateX), +1 day): for a
//    TIMESTAMP creationDate and a TIMESTAMP parameter (the runner passes datetime) the
//    two are the same condition. With $dateX specialized the bounds fold to the
//    TIMESTAMP literals of the parameter-dependent FILLs. (Kuzu rejects the nested
//    CAST(CAST($dateX AS DATE) AS TIMESTAMP) when $dateX occurs in several clauses.)
// 2. count(DISTINCT friendX) over friends with a matching message is computed per friend:
//    group the friend's matching messages, then count the distinct friends whose count
//    is positive (cf. Kuzu/queries/optimized/ldbc-bi/bi-16.cypher). A missing friend
//    or a friend without such a message contributes NULL, which count(DISTINCT) skips.
MATCH (person1:PERSON)
OPTIONAL MATCH (person1)<-[:HASCREATOR]-(messageA:POST:COMMENT)-[:HASTAG]->(:TAG {name: $tagA})
WHERE date_trunc('day', $dateA) <= messageA.creationDate
  AND messageA.creationDate < date_trunc('day', $dateA) + duration('1 day')
WITH person1, count(DISTINCT messageA) AS messageCountA
WHERE messageCountA > 0
OPTIONAL MATCH (person1)-[:KNOWS]-(friendA:PERSON)
WITH person1, messageCountA, friendA
OPTIONAL MATCH (friendA)<-[:HASCREATOR]-(friendMessageA:POST:COMMENT)-[:HASTAG]->(:TAG {name: $tagA})
WHERE date_trunc('day', $dateA) <= friendMessageA.creationDate
  AND friendMessageA.creationDate < date_trunc('day', $dateA) + duration('1 day')
WITH person1, messageCountA, friendA, count(DISTINCT friendMessageA) AS friendMessageCountA
WITH person1, messageCountA, count(DISTINCT CASE WHEN friendMessageCountA > 0 THEN friendA.id END) AS friendCountA
WHERE friendCountA <= $maxKnowsLimit
OPTIONAL MATCH (person1)<-[:HASCREATOR]-(messageB:POST:COMMENT)-[:HASTAG]->(:TAG {name: $tagB})
WHERE date_trunc('day', $dateB) <= messageB.creationDate
  AND messageB.creationDate < date_trunc('day', $dateB) + duration('1 day')
WITH person1, messageCountA, count(DISTINCT messageB) AS messageCountB
WHERE messageCountB > 0
OPTIONAL MATCH (person1)-[:KNOWS]-(friendB:PERSON)
WITH person1, messageCountA, messageCountB, friendB
OPTIONAL MATCH (friendB)<-[:HASCREATOR]-(friendMessageB:POST:COMMENT)-[:HASTAG]->(:TAG {name: $tagB})
WHERE date_trunc('day', $dateB) <= friendMessageB.creationDate
  AND friendMessageB.creationDate < date_trunc('day', $dateB) + duration('1 day')
WITH person1, messageCountA, messageCountB, friendB, count(DISTINCT friendMessageB) AS friendMessageCountB
WITH person1, messageCountA, messageCountB, count(DISTINCT CASE WHEN friendMessageCountB > 0 THEN friendB.id END) AS friendCountB
WHERE friendCountB <= $maxKnowsLimit
RETURN
  person1.id AS personId,
  messageCountA,
  messageCountB
ORDER BY messageCountA + messageCountB DESC, personId ASC
LIMIT 20
