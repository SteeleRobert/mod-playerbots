-- #########################################################
-- Playerbots - telemetry tables for the bot dashboard
--
-- The slow (LLM) strategic layer writes its decisions and the
-- notable things bots do into these two tables, which the bot
-- dashboard already knows how to read.
--
-- Both tables are OWNED by sibling modules (mod-ollama-bot-buddy
-- and mod-ollama-chat). They are created here only because a
-- deployment may run this layer INSTEAD of those modules - on
-- olab1, mod-ollama-chat predates the memory subsystem entirely
-- and never creates mod_ollama_chat_bot_events, so without this
-- the dashboard's deaths, replay and stuck panels stay dark.
--
-- CREATE TABLE IF NOT EXISTS throughout: where the owning module
-- is present and has already made the table, this is a no-op and
-- that module's definition wins.
-- #########################################################

CREATE TABLE IF NOT EXISTS `mod_ollama_bot_buddy_journal` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `bot_guid` BIGINT UNSIGNED NOT NULL,
    `bot_name` VARCHAR(24) NOT NULL,
    `ts` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `command` VARCHAR(32) NOT NULL,
    `params` TEXT,
    `reasoning` TEXT,
    `succeeded` TINYINT(1) NOT NULL DEFAULT 0,
    `outcome` TEXT,
    `latency_ms` INT UNSIGNED NOT NULL DEFAULT 0,
    `prompt` MEDIUMTEXT,
    `reply` MEDIUMTEXT,
    PRIMARY KEY (`id`),
    KEY `bot_ts` (`bot_guid`, `ts`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

CREATE TABLE IF NOT EXISTS `mod_ollama_chat_bot_events` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `bot_guid` BIGINT UNSIGNED NOT NULL,
    `timestamp` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `event_type` VARCHAR(32) NOT NULL COMMENT 'Free-form: killed, died, leveled_up, quest_done, stuck, ...',
    `actor_guid` BIGINT UNSIGNED DEFAULT NULL COMMENT 'The other party when there is one',
    `actor_name` VARCHAR(48) DEFAULT NULL COMMENT 'Denormalized so names survive deletes and renames',
    `map` SMALLINT UNSIGNED DEFAULT NULL,
    `zone` INT UNSIGNED DEFAULT NULL,
    `area` INT UNSIGNED DEFAULT NULL,
    `x` FLOAT DEFAULT NULL,
    `y` FLOAT DEFAULT NULL,
    `z` FLOAT DEFAULT NULL,
    `bot_level` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Level at event time',
    `count` INT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'Coalescing counter',
    `coalesce_key` VARCHAR(96) DEFAULT NULL COMMENT 'NULL means never coalesce',
    `detail` JSON DEFAULT NULL COMMENT 'Type-specific payload',
    `summarized` TINYINT(1) NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`),
    KEY `idx_bot_time` (`bot_guid`, `timestamp`),
    KEY `idx_bot_unsum` (`bot_guid`, `summarized`, `timestamp`),
    KEY `idx_bot_actor` (`bot_guid`, `actor_guid`),
    KEY `idx_bot_coalesce` (`bot_guid`, `event_type`, `coalesce_key`, `timestamp`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
