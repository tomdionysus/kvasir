Unicode's data, as Unicode publishes it, unchanged (ruling 32):

- allkeys-9.0.0.txt: the Default Unicode Collation Element Table for UCA 9.0.0,
  from https://www.unicode.org/Public/UCA/9.0.0/allkeys.txt
  (sha256 0633f4520c99f249b0c53aa1442cd2521702041fb00a32df944fec13c9da3ed5),
  the table MySQL 8.4's utf8mb4_0900_* collations are defined over.
- UnicodeData-3.0.0.txt: the Unicode Character Database for Unicode 3.0.0,
  from https://www.unicode.org/Public/3.0-Update/UnicodeData-3.0.0.txt
  (sha256 f41d967bc458ee106f0c3948bfad71cd0860d96c49304e3fd02eaf2bbae4b6d9),
  from which the general_ci weights are derived (ruling 33). Of the versions
  tried (3.0.0 to 9.0.0), 3.0.0's case pairs and decompositions match MySQL
  8.4's general_ci collations best: with the rule in
  `tools/kvasir_general_ci.cpp` and three exceptions, all 63,488 BMP code
  points, checked against the reference server's WEIGHT_STRING.
- LICENSE: Unicode License v3, from https://www.unicode.org/license.txt,
  whose notice must travel with the data.
