-- Performance indexes. Idempotent and DB-agnostic: runs against the current
-- database (DATABASE()), not a hardcoded schema name, and skips creation if the
-- index already exists (MySQL 8.0 has no CREATE INDEX IF NOT EXISTS).

SET @idx := (SELECT COUNT(1) FROM information_schema.statistics
             WHERE table_schema = DATABASE() AND table_name = 'auctionhouse'
               AND index_name = 'idx_ah_houseid_itemguid');
SET @s := IF(@idx = 0, 'CREATE INDEX idx_ah_houseid_itemguid ON auctionhouse(houseId, itemguid)', 'DO 0');
PREPARE st FROM @s; EXECUTE st; DEALLOCATE PREPARE st;

SET @idx2 := (SELECT COUNT(1) FROM information_schema.statistics
              WHERE table_schema = DATABASE() AND table_name = 'item_instance'
                AND index_name = 'idx_ii_guid_entry');
SET @s2 := IF(@idx2 = 0, 'CREATE INDEX idx_ii_guid_entry ON item_instance(guid, itemEntry)', 'DO 0');
PREPARE st2 FROM @s2; EXECUTE st2; DEALLOCATE PREPARE st2;
