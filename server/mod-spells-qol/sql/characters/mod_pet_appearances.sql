CREATE TABLE IF NOT EXISTS `mod_pet_appearances` (
  `owner_guid` INT(10) UNSIGNED NOT NULL,
  `appearance_id` INT(10) UNSIGNED NOT NULL AUTO_INCREMENT,
  `display_id` INT(10) UNSIGNED NOT NULL,
  `appearance_name` VARCHAR(100) NOT NULL,
  PRIMARY KEY (`appearance_id`),
  KEY `idx_owner` (`owner_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
