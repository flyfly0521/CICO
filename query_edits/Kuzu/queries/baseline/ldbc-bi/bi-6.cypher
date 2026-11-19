// Q6. Most authoritative users on a given topic
// Kuzu adaptation: Message is represented by the POST:COMMENT union.
// Query edit (IncGQ middleware), the structure of the LDBC reference query
// (Neo4j bi-6: WITH DISTINCT person1, person2 CALL (person2) {...} sum(likerScore)):
// count(DISTINCT like) per person1 = sum over its DISTINCT likers person2 of the
// likes on person2's messages, because every like edge ends at one message and
// every message has at most one creator (LDBC data model; checked on the store),
// so the like sets of different person2 are disjoint. count(person3) equals
// count(like): in the OPTIONAL MATCH both are non-null together. A NULL person2
// contributes 0, as in the original.
MATCH (tag:TAG {name: $tag})<-[:HASTAG]-(message1:POST:COMMENT)-[:HASCREATOR]->(person1:PERSON)
OPTIONAL MATCH (message1)<-[:LIKES]-(person2:PERSON)
WITH DISTINCT person1, person2
OPTIONAL MATCH (person2)<-[:HASCREATOR]-(message2:POST:COMMENT)<-[like:LIKES]-(person3:PERSON)
WITH person1, person2, count(person3) AS likerScore
RETURN
  person1.id AS personId,
  sum(likerScore) AS authorityScore
ORDER BY authorityScore DESC, personId ASC
LIMIT 100
