CREATE TABLE IF NOT EXISTS `mod_pet_transmog_unlocked` (
  `account_id` INT(10) UNSIGNED NOT NULL,
  `display_id` INT(10) UNSIGNED NOT NULL,
  `target_scale` FLOAT NOT NULL DEFAULT 1,
  `source_creature_entry` INT(10) UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`account_id`, `display_id`),
  KEY `idx_display` (`display_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
