-- #########################################################
-- Playerbots - dense position samples for journey replay
--
-- This is deliberately separate from playerbots_llm_events:
-- position samples are high-volume telemetry, not biography events.
-- Rows are pruned by LlmTelemetry according to
-- AiPlayerbot.LlmDirective.PositionRetentionDays.
-- #########################################################

CREATE TABLE IF NOT EXISTS `playerbots_llm_bot_track` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `bot_guid` BIGINT UNSIGNED NOT NULL,
    `sampled_at` DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    `map` SMALLINT UNSIGNED NOT NULL,
    `zone` INT UNSIGNED NOT NULL,
    `area` INT UNSIGNED NOT NULL,
    `x` FLOAT NOT NULL,
    `y` FLOAT NOT NULL,
    `z` FLOAT NOT NULL,
    `bot_level` TINYINT UNSIGNED NOT NULL,
    PRIMARY KEY (`id`),
    KEY `idx_bot_sampled` (`bot_guid`, `sampled_at`),
    KEY `idx_sampled_at` (`sampled_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
