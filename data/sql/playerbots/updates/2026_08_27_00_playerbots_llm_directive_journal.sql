-- #########################################################
-- Playerbots - slow (LLM) strategic layer decision journal
--
-- One row per LLM call: the exact prompt, the raw reply, how
-- long it took, whether it parsed, and the directive that came
-- out of it. Two separate bugs on this stack (a silently
-- ignored JSON schema, and a num_predict cap that truncated
-- 97% of replies) were only ever diagnosed because this table
-- kept the raw reply and its length.
--
-- The module also issues this same CREATE TABLE IF NOT EXISTS
-- at runtime when AiPlayerbot.LlmDirective.JournalAutoCreate
-- is on, so journaling still works where the SQL updater is
-- disabled.
-- #########################################################

CREATE TABLE IF NOT EXISTS `playerbots_llm_directive_journal` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `ts` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `bot_guid` BIGINT UNSIGNED NOT NULL,
    `bot_name` VARCHAR(24) NOT NULL DEFAULT '',
    `bot_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `bot_class` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `zone_id` INT UNSIGNED NOT NULL DEFAULT 0,
    `model` VARCHAR(128) NOT NULL DEFAULT '',
    `endpoint` VARCHAR(255) NOT NULL DEFAULT '',
    `num_predict` INT UNSIGNED NOT NULL DEFAULT 0,
    `latency_ms` INT UNSIGNED NOT NULL DEFAULT 0,
    `parse_ok` TINYINT(1) NOT NULL DEFAULT 0,
    `parse_error` VARCHAR(255) NOT NULL DEFAULT '',
    `reply_chars` INT UNSIGNED NOT NULL DEFAULT 0,
    `action` VARCHAR(16) NOT NULL DEFAULT '',
    `chosen_zone_id` INT UNSIGNED NOT NULL DEFAULT 0,
    `chosen_zone` VARCHAR(100) NOT NULL DEFAULT '',
    `reason` VARCHAR(512) NOT NULL DEFAULT '',
    `prev_directive` VARCHAR(128) NOT NULL DEFAULT '',
    `prev_outcome` VARCHAR(512) NOT NULL DEFAULT '',
    `prompt` MEDIUMTEXT,
    `reply` MEDIUMTEXT,
    PRIMARY KEY (`id`),
    KEY `bot_ts` (`bot_guid`, `ts`),
    KEY `parse_ok_ts` (`parse_ok`, `ts`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
