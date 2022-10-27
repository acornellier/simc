// ==========================================================================
// Dedmonwakeen's DPS-DPM Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "simulationcraft.hpp"
#include "player/covenant.hpp"
#include "util/util.hpp"

namespace { // UNNAMED NAMESPACE

// Forward Declarations
class rogue_t;

constexpr double COMBO_POINT_MAX = 5;

enum class secondary_trigger
{
  NONE = 0U,
  SINISTER_STRIKE,
  WEAPONMASTER,
  SECRET_TECHNIQUE,
  SHURIKEN_TORNADO,
  INTERNAL_BLEEDING,
  AKAARIS_SOUL_FRAGMENT,
  TRIPLE_THREAT,
  CONCEALED_BLUNDERBUSS,
  FLAGELLATION,
  MAIN_GAUCHE,
  DEATHMARK,
  VICIOUS_VENOMS,
  HIDDEN_OPPORTUNITY,
  FAN_THE_HAMMER,
};

enum stealth_type_e
{
  STEALTH_NORMAL = 0x01,
  STEALTH_VANISH = 0x02,
  STEALTH_SHADOWMELD = 0x04,
  STEALTH_SUBTERFUGE = 0x08,
  STEALTH_SHADOWDANCE = 0x10,
  STEALTH_SEPSIS = 0x20,
  STEALTH_IMPROVED_GARROTE = 0x40,

  STEALTH_ROGUE = ( STEALTH_SUBTERFUGE | STEALTH_SHADOWDANCE ),   // Subterfuge + Shadowdance
  STEALTH_BASIC = ( STEALTH_NORMAL | STEALTH_VANISH ),            // Normal + Vanish

  STEALTH_ALL = 0xFF
};

namespace actions
{
  struct rogue_attack_t;
  struct rogue_heal_t;
  struct rogue_spell_t;

  struct rogue_poison_t;
  struct melee_t;
  struct shadow_blades_attack_t;
  struct flagellation_damage_t;
}

enum current_weapon_e
{
  WEAPON_PRIMARY = 0U,
  WEAPON_SECONDARY
};

enum weapon_slot_e
{
  WEAPON_MAIN_HAND = 0U,
  WEAPON_OFF_HAND
};

struct weapon_info_t
{
  // State of the hand, i.e., primary or secondary weapon currently equipped
  current_weapon_e     current_weapon;
  // Pointers to item data
  const item_t*        item_data[ 2 ];
  // Computed weapon data
  weapon_t             weapon_data[ 2 ];
  // Computed stats data
  gear_stats_t         stats_data[ 2 ];
  // Callbacks, associated with special effects on each weapons
  std::vector<dbc_proc_callback_t*> cb_data[ 2 ];

  // Item data storage for secondary weapons
  item_t               secondary_weapon_data;

  // Protect against multiple initialization since the init is done in an action_t object.
  bool                 initialized;

  // Track secondary weapon uptime through a buff
  buff_t*              secondary_weapon_uptime;

  weapon_info_t() :
    current_weapon( WEAPON_PRIMARY ), initialized( false ), secondary_weapon_uptime( nullptr )
  {
    range::fill( item_data, nullptr );
  }

  weapon_slot_e slot() const;
  void initialize();
  void reset();

  // Enable/disable callbacks on the primary/secondary weapons.
  void callback_state( current_weapon_e weapon, bool state );
};

// ==========================================================================
// Rogue Target Data
// ==========================================================================

class rogue_td_t : public actor_target_data_t
{
  std::vector<dot_t*> bleeds;
  std::vector<dot_t*> poison_dots;
  std::vector<buff_t*> poison_debuffs;

public:
  struct dots_t
  {
    dot_t* crimson_tempest;
    dot_t* deadly_poison;
    dot_t* deadly_poison_deathmark;
    dot_t* deathmark;
    dot_t* garrote;
    dot_t* garrote_deathmark;
    dot_t* internal_bleeding;
    dot_t* killing_spree; // Strictly speaking, this should probably be on player
    dot_t* kingsbane;
    dot_t* mutilated_flesh;
    dot_t* rupture;
    dot_t* rupture_deathmark;
    dot_t* sepsis;
    dot_t* serrated_bone_spike;
  } dots;

  struct debuffs_t
  {
    buff_t* amplifying_poison;
    buff_t* amplifying_poison_deathmark;
    buff_t* atrophic_poison;
    buff_t* akaaris_soul_fragment;
    buff_t* between_the_eyes;
    buff_t* crippling_poison;
    damage_buff_t* deathmark;
    buff_t* find_weakness;
    buff_t* flagellation;
    buff_t* ghostly_strike;
    buff_t* marked_for_death;
    buff_t* numbing_poison;
    buff_t* prey_on_the_weak;
    damage_buff_t* shiv;
    buff_t* wound_poison;
    buff_t* banshees_blight; // Slyvanas Dagger
  } debuffs;

  rogue_td_t( player_t* target, rogue_t* source );

  timespan_t lethal_poison_remains() const
  {
    if ( dots.deadly_poison->is_ticking() )
      return dots.deadly_poison->remains();

    if ( dots.deadly_poison_deathmark->is_ticking() )
      return dots.deadly_poison_deathmark->remains();

    if ( debuffs.wound_poison->check() )
      return debuffs.wound_poison->remains();

    if ( debuffs.amplifying_poison->check() )
      return debuffs.amplifying_poison->remains();

    if ( debuffs.amplifying_poison_deathmark->check() )
      return debuffs.amplifying_poison_deathmark->remains();

    return 0_s;
  }

  timespan_t non_lethal_poison_remains() const
  {
    if ( debuffs.atrophic_poison->check() )
      return debuffs.atrophic_poison->remains();

    if ( debuffs.crippling_poison->check() )
      return debuffs.crippling_poison->remains();

    if ( debuffs.numbing_poison->check() )
      return debuffs.numbing_poison->remains();

    return 0_s;
  }

  bool is_lethal_poisoned() const
  {
    return dots.deadly_poison->is_ticking() || dots.deadly_poison_deathmark->is_ticking() || 
           debuffs.amplifying_poison->check() || debuffs.amplifying_poison_deathmark->check() ||
           debuffs.wound_poison->check();
  }

  bool is_non_lethal_poisoned() const
  {
    return debuffs.atrophic_poison->check() || debuffs.crippling_poison->check() || debuffs.numbing_poison->check();
  }

  bool is_poisoned() const
  {
    return is_lethal_poisoned() || is_non_lethal_poisoned();
  }

  bool is_bleeding() const
  {
    for ( auto b : bleeds )
    {
      if ( b->is_ticking() )
        return true;
    }
    return false;
  }

  int total_bleeds() const
  {
    // TOCHECK -- Confirm all these things count as intended
    return as<int>( range::count_if( bleeds, []( dot_t* dot ) { return dot->is_ticking(); } ) );
  }

  int total_poisons() const
  {
    // TOCHECK -- Confirm all these things count as intended
    return as<int>( range::count_if( poison_dots, []( dot_t* d ) { return d->is_ticking(); } ) +
                    range::count_if( poison_debuffs, []( buff_t* b ) { return b->check(); } ) );
  }
};

// ==========================================================================
// Rogue
// ==========================================================================

class rogue_t : public player_t
{
public:
  // Shadow Techniques swing counter;
  unsigned shadow_techniques_counter;

  // Danse Macabre Ability ID Tracker
  std::vector<unsigned> danse_macabre_tracker;

  // Active
  struct
  {
    actions::rogue_poison_t* lethal_poison = nullptr;
    actions::rogue_poison_t* lethal_poison_dtb = nullptr;
    actions::rogue_poison_t* nonlethal_poison = nullptr;
    actions::rogue_poison_t* nonlethal_poison_dtb = nullptr;
    
    actions::rogue_attack_t* akaaris_shadowstrike = nullptr;
    actions::rogue_attack_t* blade_flurry = nullptr;
    actions::rogue_attack_t* bloodfang = nullptr;
    actions::rogue_attack_t* fan_the_hammer = nullptr;
    actions::flagellation_damage_t* flagellation = nullptr;
    actions::rogue_attack_t* lingering_shadow = nullptr;
    actions::rogue_attack_t* main_gauche = nullptr;
    actions::rogue_attack_t* poison_bomb = nullptr;
    actions::shadow_blades_attack_t* shadow_blades_attack = nullptr;
    actions::rogue_attack_t* triple_threat_mh = nullptr;
    actions::rogue_attack_t* triple_threat_oh = nullptr;

    // Shadowlands
    actions::rogue_attack_t* concealed_blunderbuss = nullptr;
    action_t* banshees_blight = nullptr; // Slyvanas Dagger
    
    struct
    {
      actions::rogue_attack_t* backstab = nullptr;
      actions::rogue_attack_t* gloomblade = nullptr;
      actions::rogue_attack_t* shadowstrike = nullptr;
      actions::rogue_attack_t* akaaris_shadowstrike = nullptr;
    } weaponmaster;
    struct
    {
      actions::rogue_attack_t* amplifying_poison = nullptr;
      actions::rogue_attack_t* deadly_poison_dot = nullptr;
      actions::rogue_attack_t* deadly_poison_instant = nullptr;
      actions::rogue_attack_t* garrote = nullptr;
      actions::rogue_attack_t* instant_poison = nullptr;
      actions::rogue_attack_t* rupture = nullptr;
      actions::rogue_attack_t* wound_poison = nullptr;
    } deathmark;
    struct
    {
      actions::rogue_attack_t* mutilate_mh = nullptr;
      actions::rogue_attack_t* mutilate_oh = nullptr;
      actions::rogue_attack_t* ambush = nullptr;
    } vicious_venoms;
  } active;

  // Autoattacks
  action_t* auto_attack;
  actions::melee_t* melee_main_hand;
  actions::melee_t* melee_off_hand;

  // Is using stealth during combat allowed? Relevant for Dungeon sims.
  bool restealth_allowed;

  // Experimental weapon swapping
  std::array<weapon_info_t, 2> weapon_data;

  // Buffs
  struct buffs_t
  {
    // Baseline
    // Shared
    buff_t* feint;
    buff_t* shadowstep;
    buff_t* sprint;
    buff_t* stealth;
    buff_t* vanish;
    // Assassination
    buff_t* envenom;
    // Outlaw
    buff_t* adrenaline_rush;
    buff_t* blade_flurry;
    buff_t* blade_rush;
    buff_t* opportunity;
    buff_t* roll_the_bones;
    // Roll the bones buffs
    damage_buff_t* broadside;
    buff_t* buried_treasure;
    buff_t* grand_melee;
    buff_t* skull_and_crossbones;
    damage_buff_t* ruthless_precision;
    buff_t* true_bearing;
    // Subtlety
    buff_t* shadow_blades;
    damage_buff_t* shadow_dance;
    damage_buff_t* symbols_of_death;

    // Talents
    // Shared
    buff_t* alacrity;
    damage_buff_t* cold_blood;
    damage_buff_t* nightstalker;
    buff_t* subterfuge;
    buff_t* thistle_tea;

    // Assassination
    buff_t* blindside;
    damage_buff_t* elaborate_planning;
    buff_t* improved_garrote;
    buff_t* improved_garrote_aura;
    buff_t* indiscriminate_carnage_garrote;
    buff_t* indiscriminate_carnage_rupture;
    damage_buff_t* kingsbane;
    damage_buff_t* master_assassin;
    damage_buff_t* master_assassin_aura;
    buff_t* scent_of_blood;

    // Outlaw
    buff_t* audacity;
    buff_t* dreadblades;
    buff_t* killing_spree;
    buff_t* loaded_dice;
    buff_t* slice_and_dice;
    buff_t* take_em_by_surprise;
    buff_t* take_em_by_surprise_aura;
    damage_buff_t* summarily_dispatched;

    // Subtlety
    damage_buff_t* danse_macabre;
    buff_t* lingering_shadow;
    buff_t* master_of_shadows;
    buff_t* premeditation;
    buff_t* secret_technique;   // Only to simplify APL tracking
    buff_t* shadow_techniques;  // Internal tracking buff
    buff_t* shot_in_the_dark;
    buff_t* shuriken_tornado;
    buff_t* silent_storm;

    // Legendary
    damage_buff_t* deathly_shadows;
    buff_t* concealed_blunderbuss;
    damage_buff_t* finality_eviscerate;
    buff_t* finality_rupture;
    damage_buff_t* finality_black_powder;
    buff_t* greenskins_wickers;
    buff_t* guile_charm_insight_1;
    buff_t* guile_charm_insight_2;
    buff_t* guile_charm_insight_3;
    damage_buff_t* master_assassins_mark;
    damage_buff_t* master_assassins_mark_aura;
    damage_buff_t* the_rotten;

    // Covenant
    std::vector<buff_t*> echoing_reprimand;
    buff_t* flagellation;
    buff_t* flagellation_persist;
    buff_t* sepsis;

    // Conduits
    damage_buff_t* deeper_daggers;
    damage_buff_t* perforated_veins;

    // Set Bonuses
    damage_buff_t* t29_assassination_4pc;
    damage_buff_t* t29_outlaw_2pc;
    damage_buff_t* t29_outlaw_4pc;
    damage_buff_t* t29_subtlety_2pc;
    damage_buff_t* t29_subtlety_4pc;
    damage_buff_t* t29_subtlety_4pc_black_powder;

  } buffs;

  // Cooldowns
  struct cooldowns_t
  {
    cooldown_t* adrenaline_rush;
    cooldown_t* between_the_eyes;
    cooldown_t* blade_flurry;
    cooldown_t* blade_rush;
    cooldown_t* blind;
    cooldown_t* cloak_of_shadows;
    cooldown_t* cold_blood;
    cooldown_t* deathmark;
    cooldown_t* dreadblades;
    cooldown_t* echoing_reprimand;
    cooldown_t* evasion;
    cooldown_t* feint;
    cooldown_t* flagellation;
    cooldown_t* fleshcraft;
    cooldown_t* garrote;
    cooldown_t* ghostly_strike;
    cooldown_t* gouge;
    cooldown_t* grappling_hook;
    cooldown_t* indiscriminate_carnage;
    cooldown_t* keep_it_rolling;
    cooldown_t* killing_spree;
    cooldown_t* kingsbane;
    cooldown_t* marked_for_death;
    cooldown_t* riposte;
    cooldown_t* roll_the_bones;
    cooldown_t* secret_technique;
    cooldown_t* sepsis;
    cooldown_t* serrated_bone_spike;
    cooldown_t* shadow_blades;
    cooldown_t* shadow_dance;
    cooldown_t* shiv;
    cooldown_t* sprint;
    cooldown_t* symbols_of_death;
    cooldown_t* thistle_tea;
    cooldown_t* vanish;

    target_specific_cooldown_t* perforated_veins;
    target_specific_cooldown_t* weaponmaster;
  } cooldowns;

  // Gains
  struct gains_t
  {
    gain_t* adrenaline_rush;
    gain_t* adrenaline_rush_expiry;
    gain_t* blade_rush;
    gain_t* buried_treasure;
    gain_t* fatal_flourish;
    gain_t* energy_refund;
    gain_t* master_of_shadows;
    gain_t* venom_rush;
    gain_t* venomous_wounds;
    gain_t* venomous_wounds_death;
    gain_t* relentless_strikes;
    gain_t* symbols_of_death;
    gain_t* slice_and_dice;

    // CP Gains
    gain_t* seal_fate;
    gain_t* quick_draw;
    gain_t* broadside;
    gain_t* ruthlessness;
    gain_t* shadow_techniques;
    gain_t* shadow_blades;
    gain_t* serrated_bone_spike;
    gain_t* dreadblades;
    gain_t* premeditation;

    // Dragonflight
    gain_t* ace_up_your_sleeve;
    gain_t* improved_adrenaline_rush;
    gain_t* improved_adrenaline_rush_expiry;
    gain_t* improved_ambush;
    gain_t* shrouded_suffocation;
    gain_t* the_first_dance;

    // Legendary
    gain_t* dashing_scoundrel;
    gain_t* deathly_shadows;
    gain_t* the_rotten;
  } gains;

  // Spell Data
  struct spells_t
  {
    // Core Class Spells
    const spell_data_t* ambush;
    const spell_data_t* cheap_shot;
    const spell_data_t* crimson_vial;           // No implementation
    const spell_data_t* crippling_poison;
    const spell_data_t* detection;
    const spell_data_t* instant_poison;
    const spell_data_t* kick;
    const spell_data_t* kidney_shot;
    const spell_data_t* shadow_dance;
    const spell_data_t* shadowstep;
    const spell_data_t* slice_and_dice;
    const spell_data_t* sprint;
    const spell_data_t* stealth;
    const spell_data_t* vanish;
    const spell_data_t* wound_poison;

    // Class Passives
    const spell_data_t* all_rogue;
    const spell_data_t* critical_strikes;
    const spell_data_t* fleet_footed;           // DFALPHA: Duplicate passive?

    // Background Spells
    const spell_data_t* alacrity_buff;
    const spell_data_t* find_weakness_debuff;
    const spell_data_t* leeching_poison_buff;
    const spell_data_t* nightstalker_buff;
    const spell_data_t* prey_on_the_weak_debuff;
    const spell_data_t* subterfuge_buff;
    const spell_data_t* vanish_buff;

    // Cross-Expansion Override Spells
    const spell_data_t* echoing_reprimand;
    const spell_data_t* sepsis_buff;
    const spell_data_t* sepsis_expire_damage;

  } spell;

  // Spec Spell Data
  struct spec_t
  {
    // Assassination Spells
    const spell_data_t* assassination_rogue;
    const spell_data_t* envenom;
    const spell_data_t* fan_of_knives;
    const spell_data_t* garrote;
    const spell_data_t* mutilate;
    const spell_data_t* poisoned_knife;

    const spell_data_t* amplifying_poison_debuff;
    const spell_data_t* blindside_buff;
    const spell_data_t* dashing_scoundrel;
    double dashing_scoundrel_gain = 0.0;
    const spell_data_t* deadly_poison_instant;
    const spell_data_t* elaborate_planning_buff;
    const spell_data_t* improved_garrote_buff;
    const spell_data_t* improved_shiv_debuff;
    const spell_data_t* internal_bleeding_debuff;
    const spell_data_t* kingsbane_buff;
    const spell_data_t* master_assassin_buff;
    const spell_data_t* poison_bomb_driver;
    const spell_data_t* poison_bomb_damage;
    const spell_data_t* serrated_bone_spike_energize;
    const spell_data_t* scent_of_blood_buff;
    const spell_data_t* vicious_venoms_ambush;
    const spell_data_t* vicious_venoms_mutilate_mh;
    const spell_data_t* vicious_venoms_mutilate_oh;
    const spell_data_t* zoldyck_insignia;

    const spell_data_t* deathmark_debuff;
    const spell_data_t* deathmark_amplifying_poison;
    const spell_data_t* deathmark_deadly_poison_dot;
    const spell_data_t* deathmark_deadly_poison_instant;
    const spell_data_t* deathmark_garrote;
    const spell_data_t* deathmark_instant_poison;
    const spell_data_t* deathmark_rupture;
    const spell_data_t* deathmark_wound_poison;

    // Outlaw Spells
    const spell_data_t* outlaw_rogue;
    const spell_data_t* between_the_eyes;
    const spell_data_t* dispatch;
    const spell_data_t* pistol_shot;
    const spell_data_t* sinister_strike;

    const spell_data_t* audacity_buff;
    const spell_data_t* blade_flurry_attack;
    const spell_data_t* blade_flurry_instant_attack;
    const spell_data_t* blade_rush_attack;
    const spell_data_t* blade_rush_energize;
    const spell_data_t* doomblade_debuff;
    const spell_data_t* greenskins_wickers;
    const spell_data_t* greenskins_wickers_buff;
    const spell_data_t* hidden_opportunity_extra_attack;
    const spell_data_t* improved_adrenaline_rush_energize;
    const spell_data_t* killing_spree_mh_attack;
    const spell_data_t* killing_spree_oh_attack;
    const spell_data_t* opportunity_buff;
    const spell_data_t* sinister_strike_extra_attack;
    const spell_data_t* summarily_dispatched_buff;
    const spell_data_t* take_em_by_surprise_buff;
    const spell_data_t* triple_threat_attack;

    const spell_data_t* broadside;
    const spell_data_t* buried_treasure;
    const spell_data_t* grand_melee;
    const spell_data_t* skull_and_crossbones;
    const spell_data_t* ruthless_precision;
    const spell_data_t* true_bearing;

    // Subtlety Spells
    const spell_data_t* subtlety_rogue;
    const spell_data_t* backstab;
    const spell_data_t* eviscerate;
    const spell_data_t* shadow_dance;         // Baseline charge increase passive
    const spell_data_t* shadow_techniques;
    const spell_data_t* shadowstrike;
    const spell_data_t* shuriken_storm;    
    const spell_data_t* shuriken_toss;
    const spell_data_t* symbols_of_death;

    const spell_data_t* black_powder_shadow_attack;
    const spell_data_t* danse_macabre_buff;
    const spell_data_t* deeper_daggers_buff;
    const spell_data_t* eviscerate_shadow_attack;
    const spell_data_t* finality_black_powder_buff;
    const spell_data_t* finality_eviscerate_buff;
    const spell_data_t* finality_rupture_buff;
    const spell_data_t* flagellation_buff;
    const spell_data_t* flagellation_persist_buff;
    const spell_data_t* flagellation_damage;
    const spell_data_t* master_of_shadows_buff;
    const spell_data_t* perforated_veins_buff;
    const spell_data_t* premeditation_buff;
    const spell_data_t* relentless_strikes_energize;
    const spell_data_t* replicating_shadows_tick;
    const spell_data_t* invigorating_shadowdust_cdr;
    const spell_data_t* lingering_shadow_attack;
    const spell_data_t* lingering_shadow_buff;
    const spell_data_t* secret_technique_attack;
    const spell_data_t* secret_technique_clone_attack;
    const spell_data_t* shadow_blades_attack;
    const spell_data_t* shadow_focus_buff;
    const spell_data_t* shadow_techniques_energize;
    const spell_data_t* shadowstrike_stealth_buff;
    const spell_data_t* shot_in_the_dark_buff;
    const spell_data_t* silent_storm_buff;

    // Multi-Spec
    const spell_data_t* rupture; // Assassination + Subtlety

  } spec;

  // Talents
  struct talents_t
  {
    struct class_talents_t
    {
      player_talent_t shiv;
      player_talent_t blind;                    // No implementation
      player_talent_t sap;                      // No implementation

      player_talent_t evasion;                  // No implementation
      player_talent_t feint;
      player_talent_t cloak_of_shadows;         // No implementation

      player_talent_t master_poisoner;          // No implementation
      player_talent_t numbing_poison;
      player_talent_t atrophic_poison;
      player_talent_t nimble_fingers;
      player_talent_t gouge;
      player_talent_t rushed_setup;
      player_talent_t tricks_of_the_trade;      // No implementation
      player_talent_t shadowrunner;

      player_talent_t improved_wound_poison;
      player_talent_t fleet_footed;
      player_talent_t iron_stomach;             // No implementation
      player_talent_t improved_sprint;
      player_talent_t prey_on_the_weak;
      player_talent_t shadowstep;
      player_talent_t subterfuge;

      player_talent_t deadened_nerves;          // No implementation
      player_talent_t elusiveness;              // No implementation
      player_talent_t cheat_death;              // No implementation
      player_talent_t blackjack;                // No implementation

      player_talent_t deadly_precision;
      player_talent_t virulent_poisons;
      player_talent_t thiefs_versatility;
      player_talent_t tight_spender;
      player_talent_t nightstalker;
      
      player_talent_t vigor;
      player_talent_t acrobatic_strikes;
      player_talent_t improved_ambush;

      player_talent_t leeching_poison;
      player_talent_t lethality;
      player_talent_t recuperator;
      player_talent_t alacrity;
      player_talent_t soothing_darkness;        // No implementation

      player_talent_t seal_fate;
      player_talent_t cold_blood;
      player_talent_t echoing_reprimand;
      player_talent_t marked_for_death;
      player_talent_t deeper_stratagem;
      player_talent_t find_weakness;
      
      player_talent_t thistle_tea;
      player_talent_t resounding_clarity;
      player_talent_t reverberation;
      player_talent_t shadow_dance;
    } rogue;

    struct assassination_talents_t
    {
      player_talent_t deadly_poison;

      player_talent_t improved_shiv;
      player_talent_t venomous_wounds;
      player_talent_t shadowstep;

      player_talent_t cut_to_the_chase;
      player_talent_t elaborate_planning;
      player_talent_t improved_poisons;
      player_talent_t bloody_mess;
      player_talent_t internal_bleeding;
      
      player_talent_t thrown_precision;
      player_talent_t lightweight_shiv;
      player_talent_t fatal_concoction;
      player_talent_t improved_garrote;
      player_talent_t intent_to_kill;

      player_talent_t crimson_tempest;
      player_talent_t venom_rush;
      player_talent_t deathmark;
      player_talent_t master_assassin;
      player_talent_t exsanguinate;

      player_talent_t flying_daggers;
      player_talent_t vicious_venoms;
      player_talent_t lethal_dose;
      player_talent_t iron_wire;                // No implementation

      player_talent_t systemic_failure;
      player_talent_t amplifying_poison;
      player_talent_t twist_the_knife;
      player_talent_t doomblade;

      player_talent_t blindside;
      player_talent_t tiny_toxic_blade;
      player_talent_t poison_bomb;
      player_talent_t shrouded_suffocation;
      player_talent_t sepsis;
      player_talent_t serrated_bone_spike;

      player_talent_t zoldyck_recipe;
      player_talent_t dashing_scoundrel;
      player_talent_t scent_of_blood;

      player_talent_t kingsbane;
      player_talent_t dragon_tempered_blades;
      player_talent_t indiscriminate_carnage;
      
    } assassination;

    struct outlaw_talents_t
    {
      player_talent_t opportunity;
      player_talent_t blade_flurry;

      player_talent_t grappling_hook;           // No implementation
      player_talent_t weaponmaster;
      player_talent_t combat_potency;
      player_talent_t ambidexterity;
      player_talent_t hit_and_run;

      player_talent_t retractable_hook;         // No implementation
      player_talent_t combat_stamina;
      player_talent_t adrenaline_rush;
      player_talent_t riposte;                  // No implementation
      player_talent_t deft_maneuvers;           // No implementation (no dynamic range functionality)

      player_talent_t blinding_powder;          // No implementation
      player_talent_t ruthlessness;
      player_talent_t swift_slasher;
      player_talent_t restless_blades;
      player_talent_t fatal_flourish;
      player_talent_t improved_between_the_eyes;
      player_talent_t dirty_tricks;

      player_talent_t heavy_hitter;
      player_talent_t devious_stratagem;
      player_talent_t roll_the_bones;
      player_talent_t quick_draw;
      player_talent_t ace_up_your_sleeve;

      player_talent_t audacity;
      player_talent_t loaded_dice;
      player_talent_t float_like_a_butterfly;
      player_talent_t sleight_of_hand;
      player_talent_t dancing_steel;

      player_talent_t triple_threat;
      player_talent_t count_the_odds;
      player_talent_t improved_main_gauche;

      player_talent_t sepsis;
      player_talent_t ghostly_strike;
      player_talent_t blade_rush;
      player_talent_t improved_adrenaline_rush;
      player_talent_t killing_spree;
      player_talent_t dreadblades;
      player_talent_t precise_cuts;

      player_talent_t take_em_by_surprise;
      player_talent_t summarily_dispatched;
      player_talent_t fan_the_hammer;

      player_talent_t hidden_opportunity;
      player_talent_t keep_it_rolling;
      player_talent_t greenskins_wickers;

    } outlaw;

    struct subtlety_talents_t
    {
      player_talent_t improved_backstab;
      player_talent_t shadowstep;
      player_talent_t improved_shuriken_storm;

      player_talent_t weaponmaster;
      player_talent_t shadow_focus;
      player_talent_t quick_decisions;
      player_talent_t relentless_strikes;
      player_talent_t black_powder;

      player_talent_t shot_in_the_dark;
      player_talent_t premeditation;
      player_talent_t shadow_blades;
      player_talent_t silent_storm;
      player_talent_t night_terrors;            // No implementation

      player_talent_t gloomblade;
      player_talent_t improved_shadow_techniques;
      player_talent_t stiletto_staccato;
      player_talent_t veiltouched;
      player_talent_t secret_technique;

      player_talent_t swift_death;
      player_talent_t the_first_dance;
      player_talent_t master_of_shadows;
      player_talent_t deepening_shadows;
      player_talent_t replicating_shadows;

      player_talent_t shrouded_in_darkness;     // No implementation
      player_talent_t planned_execution;
      player_talent_t improved_shadow_dance;
      player_talent_t shadowed_finishers;
      player_talent_t shuriken_tornado;

      player_talent_t inevitability;
      player_talent_t without_a_trace;
      player_talent_t fade_to_nothing;          // No implementation
      player_talent_t cloaked_in_shadow;        // No implementation
      player_talent_t secret_stratagem;
      
      player_talent_t sepsis;
      player_talent_t perforated_veins;
      player_talent_t dark_shadow;
      player_talent_t deeper_daggers;
      player_talent_t flagellation;

      player_talent_t invigorating_shadowdust;
      player_talent_t lingering_shadow;
      player_talent_t finality;

      player_talent_t the_rotten;
      player_talent_t danse_macabre;
      player_talent_t dark_brew;

    } subtlety;

    struct shared_talents_t
    {
      player_talent_t sepsis;
      player_talent_t shadowstep;

    } shared;

  } talent;

  // Masteries
  struct masteries_t
  {
    // Assassination
    const spell_data_t* potent_assassin;
    // Outlaw
    const spell_data_t* main_gauche;
    const spell_data_t* main_gauche_attack;
    // Subtlety
    const spell_data_t* executioner;
  } mastery;

  // Covenant powers
  struct covenant_t
  {
    const spell_data_t* echoing_reprimand;
    const spell_data_t* flagellation;
    const spell_data_t* sepsis;
    const spell_data_t* serrated_bone_spike;
  } covenant;

  // Conduits
  struct conduit_t
  {
    conduit_data_t lethal_poisons;
    conduit_data_t maim_mangle;
    conduit_data_t poisoned_katar;
    conduit_data_t well_placed_steel;

    conduit_data_t ambidexterity;
    conduit_data_t count_the_odds;
    conduit_data_t sleight_of_hand;
    conduit_data_t triple_threat;

    conduit_data_t deeper_daggers;
    conduit_data_t perforated_veins;
    conduit_data_t planned_execution;
    conduit_data_t stiletto_staccato;

    conduit_data_t lashing_scars;
    conduit_data_t reverberation;
    conduit_data_t sudden_fractures;
    conduit_data_t septic_shock;

    conduit_data_t recuperator;
  } conduit;

  // Legendary effects
  struct legendary_t
  {
    // Generic
    item_runeforge_t essence_of_bloodfang;
    item_runeforge_t master_assassins_mark;
    item_runeforge_t tiny_toxic_blade;
    item_runeforge_t invigorating_shadowdust;

    // Generic Covenant
    item_runeforge_t deathspike;
    item_runeforge_t obedience;
    item_runeforge_t resounding_clarity;
    item_runeforge_t toxic_onslaught;

    // Assassination
    item_runeforge_t dashing_scoundrel;
    item_runeforge_t doomblade;
    item_runeforge_t duskwalkers_patch;
    item_runeforge_t zoldyck_insignia;

    // Outlaw
    item_runeforge_t greenskins_wickers;
    item_runeforge_t guile_charm;
    item_runeforge_t celerity;
    item_runeforge_t concealed_blunderbuss;

    // Subtlety
    item_runeforge_t akaaris_soul_fragment;
    item_runeforge_t deathly_shadows;
    item_runeforge_t finality;
    item_runeforge_t the_rotten;

    // Legendary Values
    double duskwalkers_patch_counter = 0.0;
    int guile_charm_counter = 0;

    // Slyvanas Dagger
    const spell_data_t* banshees_blight = nullptr;
  } legendary;

  // Procs
  struct procs_t
  {
    // Assassination
    proc_t* amplifying_poison_consumed;
    proc_t* amplifying_poison_deathmark_consumed;

    // Outlaw
    proc_t* roll_the_bones_1;
    proc_t* roll_the_bones_2;
    proc_t* roll_the_bones_3;
    proc_t* roll_the_bones_4;
    proc_t* roll_the_bones_5;
    proc_t* roll_the_bones_6;
    proc_t* roll_the_bones_wasted;

    // Subtlety
    proc_t* deepening_shadows;
    proc_t* weaponmaster;

    // Covenant
    std::vector<proc_t*> echoing_reprimand;
    proc_t* flagellation_cp_spend;
    proc_t* serrated_bone_spike_refund;
    proc_t* serrated_bone_spike_waste;
    proc_t* serrated_bone_spike_waste_partial;

    // Conduits
    proc_t* count_the_odds;
    proc_t* count_the_odds_capped;

    // Legendary
    proc_t* duskwalker_patch;

    // Set Bonus

  } procs;

  // Set Bonus effects
  struct set_bonuses_t
  {
    const spell_data_t* t29_assassination_2pc;
    const spell_data_t* t29_assassination_4pc;
    const spell_data_t* t29_outlaw_2pc;
    const spell_data_t* t29_outlaw_4pc;
    const spell_data_t* t29_subtlety_2pc;
    const spell_data_t* t29_subtlety_4pc;
  } set_bonuses;

  // Options
  struct rogue_options_t
  {
    std::vector<size_t> fixed_rtb;
    std::vector<double> fixed_rtb_odds;
    int initial_combo_points = 0;
    int initial_shadow_techniques = -1;
    bool rogue_ready_trigger = true;
    bool prepull_shadowdust = false;
    bool priority_rotation = false;
  } options;

  rogue_t( sim_t* sim, util::string_view name, race_e r = RACE_NIGHT_ELF ) :
    player_t( sim, ROGUE, name, r ),
    shadow_techniques_counter( 0 ),
    auto_attack( nullptr ), melee_main_hand( nullptr ), melee_off_hand( nullptr ),
    restealth_allowed( false ),
    buffs( buffs_t() ),
    cooldowns( cooldowns_t() ),
    gains( gains_t() ),
    spell( spells_t() ),
    spec( spec_t() ),
    talent( talents_t() ),
    mastery( masteries_t() ),
    covenant( covenant_t() ),
    conduit( conduit_t() ),
    legendary( legendary_t() ),
    procs( procs_t() ),
    set_bonuses( set_bonuses_t() ),
    options( rogue_options_t() )
  {
    // Cooldowns
    cooldowns.adrenaline_rush           = get_cooldown( "adrenaline_rush" );
    cooldowns.between_the_eyes          = get_cooldown( "between_the_eyes" );
    cooldowns.blade_flurry              = get_cooldown( "blade_flurry" );
    cooldowns.blade_rush                = get_cooldown( "blade_rush" );
    cooldowns.blind                     = get_cooldown( "blind" );
    cooldowns.cloak_of_shadows          = get_cooldown( "cloak_of_shadows" );
    cooldowns.cold_blood                = get_cooldown( "cold_blood" );
    cooldowns.deathmark                 = get_cooldown( "deathmark" );
    cooldowns.dreadblades               = get_cooldown( "dreadblades" );
    cooldowns.echoing_reprimand         = get_cooldown( "echoing_reprimand" );
    cooldowns.evasion                   = get_cooldown( "evasion" );
    cooldowns.feint                     = get_cooldown( "feint" );
    cooldowns.flagellation              = get_cooldown( "flagellation" );
    cooldowns.fleshcraft                = get_cooldown( "fleshcraft" );
    cooldowns.garrote                   = get_cooldown( "garrote" );
    cooldowns.ghostly_strike            = get_cooldown( "ghostly_strike" );
    cooldowns.gouge                     = get_cooldown( "gouge" );
    cooldowns.grappling_hook            = get_cooldown( "grappling_hook" );
    cooldowns.indiscriminate_carnage    = get_cooldown( "indiscriminate_carnage" );
    cooldowns.keep_it_rolling           = get_cooldown( "keep_it_rolling" );
    cooldowns.killing_spree             = get_cooldown( "killing_spree" );
    cooldowns.kingsbane                 = get_cooldown( "kingsbane" );
    cooldowns.marked_for_death          = get_cooldown( "marked_for_death" );
    cooldowns.riposte                   = get_cooldown( "riposte" );
    cooldowns.roll_the_bones            = get_cooldown( "roll_the_bones" );
    cooldowns.secret_technique          = get_cooldown( "secret_technique" );
    cooldowns.sepsis                    = get_cooldown( "sepsis" );
    cooldowns.serrated_bone_spike       = get_cooldown( "serrated_bone_spike" );
    cooldowns.shadow_blades             = get_cooldown( "shadow_blades" );
    cooldowns.shadow_dance              = get_cooldown( "shadow_dance" );
    cooldowns.shiv                      = get_cooldown( "shiv" );
    cooldowns.sprint                    = get_cooldown( "sprint" );   
    cooldowns.symbols_of_death          = get_cooldown( "symbols_of_death" );
    cooldowns.thistle_tea               = get_cooldown( "thistle_tea" );
    cooldowns.vanish                    = get_cooldown( "vanish" );

    cooldowns.perforated_veins          = get_target_specific_cooldown( "perforated_veins" );
    cooldowns.weaponmaster              = get_target_specific_cooldown( "weaponmaster" );

    resource_regeneration = regen_type::DYNAMIC;
    regen_caches[CACHE_HASTE] = true;
    regen_caches[CACHE_ATTACK_HASTE] = true;
  }

  // Character Definition
  void        init_spells() override;
  void        init_base_stats() override;
  void        init_gains() override;
  void        init_procs() override;
  void        init_scaling() override;
  void        init_resources( bool force ) override;
  void        init_items() override;
  void        init_special_effects() override;
  void        init_finished() override;
  void        create_buffs() override;
  void        create_options() override;
  void        copy_from( player_t* source ) override;
  std::string create_profile( save_e stype ) override;
  void        init_action_list() override;
  void        reset() override;
  void        activate() override;
  void        arise() override;
  void        combat_begin() override;
  timespan_t  available() const override;
  action_t*   create_action( util::string_view name, util::string_view options ) override;
  std::unique_ptr<expr_t> create_expression( util::string_view name_str ) override;
  std::unique_ptr<expr_t> create_resource_expression( util::string_view name ) override;
  void        regen( timespan_t periodicity ) override;
  resource_e  primary_resource() const override { return RESOURCE_ENERGY; }
  role_e      primary_role() const override  { return ROLE_ATTACK; }
  stat_e      convert_hybrid_stat( stat_e s ) const override;

  // Default consumables
  std::string default_potion() const override;
  std::string default_flask() const override;
  std::string default_food() const override;
  std::string default_rune() const override;
  std::string default_temporary_enchant() const override;

  double    composite_attribute_multiplier( attribute_e attr ) const override;
  double    composite_melee_speed() const override;
  double    composite_melee_haste() const override;
  double    composite_melee_crit_chance() const override;
  double    composite_spell_crit_chance() const override;
  double    composite_spell_haste() const override;
  double    composite_damage_versatility() const override;
  double    composite_heal_versatility() const override;
  double    composite_leech() const override;
  double    matching_gear_multiplier( attribute_e attr ) const override;
  double    composite_player_multiplier( school_e school ) const override;
  double    composite_player_target_multiplier( player_t* target, school_e school ) const override;
  double    composite_player_target_crit_chance( player_t* target ) const override;
  double    composite_player_target_armor( player_t* target ) const override;
  double    resource_regen_per_second( resource_e ) const override;
  double    passive_movement_modifier() const override;
  double    temporary_movement_modifier() const override;
  void      apply_affecting_auras( action_t& action ) override;

  void break_stealth();
  void cancel_auto_attacks() override;
  void do_exsanguinate( dot_t* dot, double coeff );

  void trigger_venomous_wounds_death( player_t* ); // On-death trigger for Venomous Wounds energy replenish
  void trigger_toxic_onslaught( player_t* );
  void trigger_exsanguinate( player_t* );

  double consume_cp_max() const
  {
    return COMBO_POINT_MAX + as<double>( talent.rogue.deeper_stratagem->effectN( 2 ).base_value() +
                                         talent.outlaw.devious_stratagem->effectN( 2 ).base_value() +
                                         talent.subtlety.secret_stratagem->effectN( 2 ).base_value() );
  }

  double current_cp( bool react = false ) const
  {
    if ( react )
    {
      // If we need to react to Shadow Techniques, check to see if the tracking buff is in the react state
      if ( buffs.shadow_techniques->check() && !buffs.shadow_techniques->stack_react() )
      {
        // Previous CP value is cached in the buff value in trigger_shadow_techniques()
        return buffs.shadow_techniques->check_value();
      }
    }

    return resources.current[ RESOURCE_COMBO_POINT ];
  }

  // Current number of effective combo points, considering Echoing Reprimand
  double current_effective_cp( bool use_echoing_reprimand = true, bool react = false ) const
  {
    double current_cp = this->current_cp( react );

    if ( use_echoing_reprimand && current_cp > 0 )
    {
      if ( range::any_of( buffs.echoing_reprimand, [ current_cp ]( const buff_t* buff ) { return buff->check_value() == current_cp; } ) )
        return spell.echoing_reprimand->effectN( 2 ).base_value();
    }

    return current_cp;
  }

  // Extra attack proc chance for Outlaw mechanics (Sinister Strike, Audacity, Hidden Opportunity)
  double extra_attack_proc_chance() const
  {
    double proc_chance = spec.sinister_strike->effectN( 3 ).percent();
    proc_chance += talent.outlaw.weaponmaster->effectN( 1 ).percent();
    proc_chance += buffs.skull_and_crossbones->stack_value();
    return proc_chance;
  }

  target_specific_t<rogue_td_t> target_data;

  const rogue_td_t* find_target_data( const player_t* target ) const override
  {
    return target_data[ target ];
  }

  rogue_td_t* get_target_data( player_t* target ) const override
  {
    rogue_td_t*& td = target_data[ target ];
    if ( ! td )
    {
      td = new rogue_td_t( target, const_cast<rogue_t*>(this) );
    }
    return td;
  }

  static actions::rogue_attack_t* cast_attack( action_t* action )
  { return debug_cast<actions::rogue_attack_t*>( action ); }

  static const actions::rogue_attack_t* cast_attack( const action_t* action )
  { return debug_cast<const actions::rogue_attack_t*>( action ); }

  void swap_weapon( weapon_slot_e slot, current_weapon_e to_weapon, bool in_combat = true );
  bool stealthed( uint32_t stealth_mask = STEALTH_ALL ) const;

  // Secondary Action Tracking
private:
  std::vector<action_t*> background_actions;

public:
  template <typename T, typename... Ts>
  T* find_background_action( util::string_view n = {} )
  {
    T* found_action = nullptr;
    for ( auto action : background_actions )
    {
      found_action = dynamic_cast<T*>( action );
      if ( found_action )
      {
        if ( n.empty() || found_action->name_str == n )
          break;
        else
          found_action = nullptr;
      }
    }
    return found_action;
  }

  template <typename T, typename... Ts>
  T* get_background_action( util::string_view n, Ts&&... args )
  {
    auto it = range::find( background_actions, n, &action_t::name_str );
    if ( it != background_actions.cend() )
    {
      return dynamic_cast<T*>( *it );
    }

    auto action = new T( n, this, std::forward<Ts>( args )... );
    action->background = true;
    background_actions.push_back( action );
    return action;
  }

  // Secondary Action Trigger Tracking
private:
  std::vector<action_t*> secondary_trigger_actions;

public:
  template <typename T, typename... Ts>
  T* find_secondary_trigger_action( secondary_trigger source, util::string_view n = {} )
  {
    T* found_action = nullptr;
    for ( auto action : secondary_trigger_actions )
    {
      found_action = dynamic_cast<T*>( action );
      if ( found_action )
      {
        if ( found_action->secondary_trigger_type == source && ( n.empty() || found_action->name_str == n ) )
          break;
        else
          found_action = nullptr;
      }
    }
    return found_action;
  }

  template <typename T, typename... Ts>
  T* get_secondary_trigger_action( secondary_trigger source, util::string_view n, Ts&&... args )
  {
    T* found_action = find_secondary_trigger_action<T>( source, n );
    if ( !found_action )
    {
      // Actions will follow form of foo_t( util::string_view name, rogue_t* p, ... )
      found_action = new T( n, this, std::forward<Ts>( args )... );
      found_action->background = found_action->dual = true;
      found_action->repeating = false;
      found_action->secondary_trigger_type = source;
      secondary_trigger_actions.push_back( found_action );
    }
    return found_action;
  }
};

namespace actions { // namespace actions

// ==========================================================================
// Secondary Action Triggers
// ==========================================================================

template<typename Base>
struct secondary_action_trigger_t : public event_t
{
  Base* action;
  action_state_t* state;
  player_t* target;
  int cp;

  secondary_action_trigger_t( action_state_t* s, timespan_t delay = timespan_t::zero() ) :
    event_t( *s -> action -> sim, delay ), action( dynamic_cast<Base*>( s->action ) ), state( s ), target( nullptr ), cp( 0 )
  {}

  secondary_action_trigger_t( player_t* target, Base* action, int cp, timespan_t delay = timespan_t::zero() ) :
    event_t( *action -> sim, delay ), action( action ), state( nullptr ), target( target ), cp( cp )
  {}

  const char* name() const override
  { return "secondary_action_trigger"; }

  void execute() override
  {
    assert( action->is_secondary_action() );

    player_t* action_target = state ? state->target : target;

    // Ensure target is still available and did not demise during delay.
    if ( !action_target || action_target->is_sleeping() )
      return;

    action->set_target( action_target );

    // No state, construct one and grab combo points from the event instead of current CP amount.
    if ( !state )
    {
      state = action->get_state();
      state->target = action_target;
      action->cast_state( state )->set_combo_points( cp, cp );
      // Calling snapshot_internal, snapshot_state would overwrite CP.
      action->snapshot_internal( state, action->snapshot_flags, action->amount_type( state ) );
    }
    
    assert( !action->pre_execute_state );

    action->pre_execute_state = state;
    action->execute();
    state = nullptr;
  }

  ~secondary_action_trigger_t() override
  { if ( state ) action_state_t::release( state ); }
};

// ==========================================================================
// Rogue Action State
// ==========================================================================

template<typename T_ACTION>
struct rogue_action_state_t : public action_state_t
{
private:
  T_ACTION* action;
  int base_cp;
  int total_cp;
  double exsanguinated_rate;
  bool exsanguinated;

public:
  rogue_action_state_t( action_t* action, player_t* target ) :
    action_state_t( action, target ),
    action( dynamic_cast<T_ACTION*>( action ) ),
    base_cp( 0 ), total_cp( 0 ), exsanguinated_rate( 1.0 ), exsanguinated( false )
  {}

  void initialize() override
  {
    action_state_t::initialize();
    base_cp = 0;
    total_cp = 0;
    exsanguinated_rate = 1.0;
    exsanguinated = false;
  }

  std::ostringstream& debug_str( std::ostringstream& s ) override
  {
    action_state_t::debug_str( s ) << " base_cp=" << base_cp << " total_cp=" << total_cp << " exsanguinated_rate=" << exsanguinated_rate;
    return s;
  }

  void copy_state( const action_state_t* s ) override
  {
    action_state_t::copy_state( s );
    const rogue_action_state_t* rs = debug_cast<const rogue_action_state_t*>( s );
    base_cp = rs->base_cp;
    total_cp = rs->total_cp;
    exsanguinated_rate = rs->exsanguinated_rate;
    exsanguinated = rs->exsanguinated;
  }

  T_ACTION* get_action() const
  {
    return action;
  }

  void set_combo_points( int base_cp, int total_cp )
  {
    this->base_cp = base_cp;
    this->total_cp = total_cp;
  }

  int get_combo_points( bool base_only = false ) const
  {
    if ( base_only )
      return base_cp;

    return total_cp;
  }

  void set_exsanguinated_rate( double rate )
  {
    exsanguinated_rate = rate;
    exsanguinated = true;
  }

  double get_exsanguinated_rate() const
  {
    return exsanguinated_rate;
  }

  double is_exsanguinated() const
  {
    return exsanguinated;
  }

  void clear_exsanguinated()
  {
    exsanguinated_rate = 1.0;
    exsanguinated = false;
  }

  proc_types2 cast_proc_type2() const override
  {
    if( action->secondary_trigger_type == secondary_trigger::WEAPONMASTER )
    {
      return PROC2_CAST_DAMAGE;
    }

    return action_state_t::cast_proc_type2();
  }
};

// ==========================================================================
// Rogue Action Template
// ==========================================================================

template <typename Base>
class rogue_action_t : public Base
{
protected:
  /// typedef for rogue_action_t<action_base_t>
  using base_t = rogue_action_t<Base>;

private:
  /// typedef for the templated action type, eg. spell_t, attack_t, heal_t
  using ab = Base;
  bool _requires_stealth;
  bool _breaks_stealth;

public:
  // Secondary triggered ability, due to Weaponmaster talent or Death from Above. Secondary
  // triggered abilities cost no resources or incur cooldowns.
  secondary_trigger secondary_trigger_type;

  proc_t* animacharged_cp_proc;
  proc_t* cold_blood_consumed_proc;

  // Affect flags for various dynamic effects
  struct damage_affect_data
  {
    bool direct = false;
    double direct_percent = 0.0;
    bool periodic = false;
    double periodic_percent = 0.0;
  };
  struct
  {
    // Shadowlands
    bool flagellation = false;
    bool dashing_scoundrel = false;
    bool zoldyck_insignia = false;
    bool sepsis = false;                // Stance Mask

    // Dragonflight
    bool adrenaline_rush_gcd = false;
    bool alacrity = false;
    bool audacity = false;              // Stance Mask
    bool blindside = false;             // Stance Mask
    bool broadside_cp = false;
    bool cold_blood = false;
    bool danse_macabre = false;         // Trigger
    bool deathmark = false;             // Tuning Aura
    bool deepening_shadows = false;     // Trigger
    bool elaborate_planning = false;    // Trigger
    bool improved_ambush = false;
    bool improved_shiv = false;
    bool lethal_dose = false;
    bool maim_mangle = false;           // Renamed Systemic Failure for DF talent
    bool master_assassin = false;
    bool nightstalker = false;
    bool relentless_strikes = false;    // Trigger
    bool ruthlessness = false;          // Trigger
    bool shadow_blades_cp = false;
    bool the_rotten = false;            // Crit Bonus

    bool t29_assassination_2pc = false;

    damage_affect_data mastery_executioner;
    damage_affect_data mastery_potent_assassin;
  } affected_by;

  std::vector<damage_buff_t*> direct_damage_buffs;
  std::vector<damage_buff_t*> periodic_damage_buffs;
  std::vector<damage_buff_t*> auto_attack_damage_buffs;
  std::vector<damage_buff_t*> crit_chance_buffs;
  
  std::vector<std::pair<buff_t*,proc_t*>> consume_buffs;

  // Init =====================================================================

  rogue_action_t( util::string_view n, rogue_t* p, const spell_data_t* s = spell_data_t::nil(),
                  util::string_view options = {} )
    : ab( n, p, s ),
    _requires_stealth( false ),
    _breaks_stealth( true ),
    secondary_trigger_type( secondary_trigger::NONE ),
    animacharged_cp_proc( nullptr ),
    cold_blood_consumed_proc( nullptr )
  {
    ab::parse_options( options );
    parse_spell_data( s );

    // rogue_t sets base and min GCD to 1s by default but let's also enforce non-hasted GCDs.
    // Even for rogue abilities that can be considered spells, hasted GCDs seem to be an exception rather than rule.
    // Those should be set explicitly. (see Vendetta, Shadow Blades, Detection)
    ab::gcd_type = gcd_haste_type::NONE;
    
    // Affecting Passive Spells
    ab::apply_affecting_aura( p->spec.shadow_dance );

    // Affecting Passive Talents
    ab::apply_affecting_aura( p->talent.rogue.master_poisoner );
    ab::apply_affecting_aura( p->talent.rogue.nimble_fingers );
    ab::apply_affecting_aura( p->talent.rogue.rushed_setup );
    ab::apply_affecting_aura( p->talent.rogue.improved_sprint );
    ab::apply_affecting_aura( p->talent.rogue.shadowstep );
    ab::apply_affecting_aura( p->talent.rogue.deadly_precision );
    ab::apply_affecting_aura( p->talent.rogue.virulent_poisons );
    ab::apply_affecting_aura( p->talent.rogue.tight_spender );
    ab::apply_affecting_aura( p->talent.rogue.acrobatic_strikes );
    ab::apply_affecting_aura( p->talent.rogue.lethality );
    ab::apply_affecting_aura( p->talent.rogue.deeper_stratagem );
    ab::apply_affecting_aura( p->talent.rogue.reverberation );
    ab::apply_affecting_aura( p->talent.rogue.shadow_dance );

    ab::apply_affecting_aura( p->talent.assassination.bloody_mess );
    ab::apply_affecting_aura( p->talent.assassination.thrown_precision );
    ab::apply_affecting_aura( p->talent.assassination.lightweight_shiv );
    ab::apply_affecting_aura( p->talent.assassination.fatal_concoction );
    ab::apply_affecting_aura( p->talent.assassination.flying_daggers );
    ab::apply_affecting_aura( p->talent.assassination.tiny_toxic_blade );
    ab::apply_affecting_aura( p->talent.assassination.shrouded_suffocation );

    ab::apply_affecting_aura( p->talent.outlaw.blinding_powder );
    ab::apply_affecting_aura( p->talent.outlaw.improved_between_the_eyes );
    ab::apply_affecting_aura( p->talent.outlaw.dirty_tricks );
    ab::apply_affecting_aura( p->talent.outlaw.heavy_hitter );
    ab::apply_affecting_aura( p->talent.outlaw.devious_stratagem );

    ab::apply_affecting_aura( p->talent.subtlety.improved_backstab );
    ab::apply_affecting_aura( p->talent.subtlety.improved_shuriken_storm );
    ab::apply_affecting_aura( p->talent.subtlety.quick_decisions );
    ab::apply_affecting_aura( p->talent.subtlety.swift_death );
    ab::apply_affecting_aura( p->talent.subtlety.replicating_shadows );
    ab::apply_affecting_aura( p->talent.subtlety.without_a_trace );
    ab::apply_affecting_aura( p->talent.subtlety.secret_stratagem );
    ab::apply_affecting_aura( p->talent.subtlety.dark_brew );

    ab::apply_affecting_aura( p->talent.shared.shadowstep );

    // Affecting Passive Auras
    // Put ability specific ones here; class/spec wide ones with labels that can effect things like trinkets in rogue_t::apply_affecting_auras.
    ab::apply_affecting_aura( p->legendary.tiny_toxic_blade );
    ab::apply_affecting_aura( p->legendary.deathspike );

    // Affecting Passive Conduits
    ab::apply_affecting_conduit( p->conduit.lashing_scars );
    ab::apply_affecting_conduit( p->conduit.lethal_poisons );
    ab::apply_affecting_conduit( p->conduit.poisoned_katar );
    ab::apply_affecting_conduit( p->conduit.reverberation );

    // Dynamically affected flags
    // Special things like CP, Energy, Crit, etc.
    affected_by.shadow_blades_cp = ab::data().affected_by( p->talent.subtlety.shadow_blades->effectN( 2 ) ) ||
      ab::data().affected_by( p->talent.subtlety.shadow_blades->effectN( 3 ) );
    affected_by.adrenaline_rush_gcd = ab::data().affected_by( p->talent.outlaw.adrenaline_rush->effectN( 3 ) );
    
    affected_by.broadside_cp = ab::data().affected_by( p->spec.broadside->effectN( 1 ) ) ||
      ab::data().affected_by( p->spec.broadside->effectN( 2 ) ) ||
      ab::data().affected_by( p->spec.broadside->effectN( 3 ) );

    // Cross-Expansion Mechanics
    if ( p->conduit.maim_mangle->ok() || p->talent.assassination.systemic_failure->ok() )
    {
      affected_by.maim_mangle = ab::data().affected_by( p->spec.garrote->effectN( 4 ) );
    }

    if ( p->legendary.dashing_scoundrel.ok() || p->talent.assassination.dashing_scoundrel->ok() )
    {
      affected_by.dashing_scoundrel = ab::data().affected_by( p->spec.envenom->effectN( 3 ) );
    }

    if ( p->spell.sepsis_buff->ok() )
    {
      affected_by.sepsis = ab::data().affected_by( p->spell.sepsis_buff->effectN( 1 ) );
    }

    if ( p->legendary.zoldyck_insignia.ok() || p->talent.assassination.zoldyck_recipe->ok() )
    {
      // Not in spell data. Using the mastery whitelist as a baseline, most seem to apply
      affected_by.zoldyck_insignia = ab::data().affected_by( p->mastery.potent_assassin->effectN( 1 ) ) ||
        ab::data().affected_by( p->mastery.potent_assassin->effectN( 2 ) );
    }

    // Dragonflight
    affected_by.audacity = ab::data().affected_by( p->spec.audacity_buff->effectN( 1 ) );
    affected_by.blindside = ab::data().affected_by( p->spec.blindside_buff->effectN( 1 ) );
    affected_by.danse_macabre = ab::data().affected_by( p->spec.danse_macabre_buff->effectN( 1 ) );
    affected_by.improved_ambush = ab::data().affected_by( p->talent.rogue.improved_ambush->effectN( 1 ) );
    affected_by.improved_shiv = ab::data().affected_by( p->spec.improved_shiv_debuff->effectN( 1 ) );
    affected_by.master_assassin = ab::data().affected_by( p->spec.master_assassin_buff->effectN( 1 ) );

    if ( p->talent.assassination.lethal_dose->ok() )
    {
      // TOCHECK -- Not in spell data. Using the mastery whitelist as a baseline, most seem to apply
      affected_by.lethal_dose = ab::data().affected_by( p->mastery.potent_assassin->effectN( 1 ) ) ||
                                ab::data().affected_by( p->mastery.potent_assassin->effectN( 2 ) );
    }

    if ( p->talent.assassination.deathmark->ok() )
    {
      affected_by.deathmark = ab::data().affected_by( p->spec.deathmark_debuff->effectN( 1 ) ) ||
                              ab::data().affected_by( p->spec.deathmark_debuff->effectN( 2 ) );
    }

    if ( p->talent.subtlety.the_rotten->ok() )
    {
      affected_by.the_rotten = ab::data().affected_by( p->talent.subtlety.the_rotten->effectN( 1 ).trigger()
                                                       ->effectN( 4 ) );
    }

    if ( p->set_bonuses.t29_assassination_2pc->ok() )
    {
      affected_by.t29_assassination_2pc = ab::data().affected_by( p->spec.envenom->effectN( 7 ) );
    }

    // Auto-parsing for damage affecting dynamic flags, this reads IF direct/periodic dmg is affected and stores by how much.
    // Still requires manual impl below but removes need to hardcode effect numbers.
    parse_damage_affecting_spell( p->mastery.executioner, affected_by.mastery_executioner );
    parse_damage_affecting_spell( p->mastery.potent_assassin, affected_by.mastery_potent_assassin );
  }

  void init() override
  {
    ab::init();
    
    if ( consumes_echoing_reprimand() )
    {
      animacharged_cp_proc = p()->get_proc( "Echoing Reprimand " + ab::name_str );
    }

    auto register_damage_buff = [ this ]( damage_buff_t* buff ) {
      if ( buff->is_affecting_direct( ab::s_data ) )
        direct_damage_buffs.push_back( buff );

      if ( buff->is_affecting_periodic( ab::s_data ) )
        periodic_damage_buffs.push_back( buff );

      if ( ab::repeating && !ab::special && !ab::s_data->ok() && buff->auto_attack_mod.multiplier != 1.0 )
        auto_attack_damage_buffs.push_back( buff );

      if ( buff->is_affecting_crit_chance( ab::s_data ) )
        crit_chance_buffs.push_back( buff );
    };

    direct_damage_buffs.clear();
    periodic_damage_buffs.clear();
    auto_attack_damage_buffs.clear();
    crit_chance_buffs.clear();

    register_damage_buff( p()->buffs.broadside );
    register_damage_buff( p()->buffs.cold_blood );
    register_damage_buff( p()->buffs.danse_macabre );
    register_damage_buff( p()->buffs.deathly_shadows );
    register_damage_buff( p()->buffs.elaborate_planning );
    register_damage_buff( p()->buffs.finality_eviscerate );
    register_damage_buff( p()->buffs.finality_black_powder );
    register_damage_buff( p()->buffs.kingsbane );
    register_damage_buff( p()->buffs.master_assassin );
    register_damage_buff( p()->buffs.master_assassin_aura );
    register_damage_buff( p()->buffs.master_assassins_mark );
    register_damage_buff( p()->buffs.master_assassins_mark_aura );
    register_damage_buff( p()->buffs.perforated_veins );
    register_damage_buff( p()->buffs.ruthless_precision );
    register_damage_buff( p()->buffs.shadow_dance );
    register_damage_buff( p()->buffs.summarily_dispatched );
    register_damage_buff( p()->buffs.symbols_of_death );
    register_damage_buff( p()->buffs.the_rotten );

    register_damage_buff( p()->buffs.t29_assassination_4pc );
    register_damage_buff( p()->buffs.t29_outlaw_2pc );
    register_damage_buff( p()->buffs.t29_outlaw_4pc );
    register_damage_buff( p()->buffs.t29_subtlety_2pc );
    register_damage_buff( p()->buffs.t29_subtlety_4pc );
    register_damage_buff( p()->buffs.t29_subtlety_4pc_black_powder );

    // Dragonflight version of Deeper Daggers is not a whitelisted buff and still a school buff
    if ( !p()->talent.subtlety.deeper_daggers->ok() && p()->conduit.deeper_daggers.ok() )
    {
      // 2022-08-04 -- S4 hotfixed whitelist data is incomplete, manually add R2 spells
      //               Shadow Blades also works but this is handled elsewhere
      register_damage_buff( p()->buffs.deeper_daggers );
      if ( ab::data().id() == 328082 || ab::data().id() == 319190 )
      {
        direct_damage_buffs.push_back( p()->buffs.deeper_daggers );
      }
    }

    if ( p()->talent.rogue.nightstalker->ok() )
    {
      affected_by.nightstalker = p()->buffs.nightstalker->is_affecting( ab::s_data );
    }

    if ( ab::base_costs[ RESOURCE_COMBO_POINT ] > 0 )
    {
      affected_by.alacrity = true;
      affected_by.deepening_shadows = true;
      affected_by.elaborate_planning = true;
      affected_by.flagellation = true;
      affected_by.relentless_strikes = true;
      affected_by.ruthlessness = true;
    }

    // Auto-Consume Buffs on Execute
    auto register_consume_buff = [this]( buff_t* buff, bool condition, proc_t* proc = nullptr ) {
      if ( condition )
      {
        consume_buffs.emplace_back( buff, proc );
      }
    };

    register_consume_buff( p()->buffs.audacity, affected_by.audacity );
    register_consume_buff( p()->buffs.blindside, affected_by.blindside );
    register_consume_buff( p()->buffs.cold_blood,
                           p()->buffs.cold_blood->is_affecting( &ab::data() ), cold_blood_consumed_proc );
    register_consume_buff( p()->buffs.t29_outlaw_2pc, p()->buffs.t29_outlaw_2pc->is_affecting( &ab::data() ) );
    register_consume_buff( p()->buffs.t29_outlaw_4pc, p()->buffs.t29_outlaw_4pc->is_affecting( &ab::data() ) );
    register_consume_buff( p()->buffs.t29_subtlety_2pc, p()->buffs.t29_subtlety_2pc->is_affecting( &ab::data() ) );
  }

  // Type Wrappers ============================================================

  static const rogue_action_state_t<base_t>* cast_state( const action_state_t* st )
  { return debug_cast<const rogue_action_state_t<base_t>*>( st ); }

  static rogue_action_state_t<base_t>* cast_state( action_state_t* st )
  { return debug_cast<rogue_action_state_t<base_t>*>( st ); }

  rogue_t* p()
  { return debug_cast<rogue_t*>( ab::player ); }

  const rogue_t* p() const
  { return debug_cast<const rogue_t*>( ab::player ); }

  rogue_td_t* td( player_t* t ) const
  { return p()->get_target_data( t ); }

  // Spell Data Helpers =======================================================

  void parse_spell_data( const spell_data_t* s )
  {
    if ( s->stance_mask() & 0x20000000)
      _requires_stealth = true;

    if ( s->flags( spell_attribute::SX_NO_STEALTH_BREAK ) )
      _breaks_stealth = false;

    for ( size_t i = 1; i <= s->effect_count(); i++ )
    {
      const spelleffect_data_t& effect = s->effectN( i );

      switch ( effect.type() )
      {
        case E_ADD_COMBO_POINTS:
          if ( ab::energize_type != action_energize::NONE )
          {
            ab::energize_type = action_energize::ON_HIT;
            ab::energize_amount = effect.base_value();
            ab::energize_resource = RESOURCE_COMBO_POINT;
          }
          break;
        default:
          break;
      }

      if ( effect.type() == E_APPLY_AURA && effect.subtype() == A_PERIODIC_DAMAGE )
      {
        ab::base_ta_adder = effect.bonus( p() );
      }
      else if ( effect.type() == E_SCHOOL_DAMAGE )
      {
        ab::base_dd_adder = effect.bonus( p() );
      }
    }
  }

  void parse_damage_affecting_spell( const spell_data_t* spell, damage_affect_data& flags )
  {
    for ( const spelleffect_data_t& effect : spell->effects() )
    {
      if ( !effect.ok() || effect.type() != E_APPLY_AURA || effect.subtype() != A_ADD_PCT_MODIFIER )
        continue;

      if ( ab::data().affected_by( effect ) )
      {
        switch ( effect.misc_value1() )
        {
          case P_GENERIC:
            flags.direct = true;
            flags.direct_percent = effect.percent();
            break;
          case P_TICK_DAMAGE:
            flags.periodic = true;
            flags.periodic_percent = effect.percent();
            break;
        }
      }
    }
  }

  // Action State =============================================================

  action_state_t* new_state() override
  {
    return new rogue_action_state_t<base_t>( this, ab::target );
  }

  void update_state( action_state_t* state, unsigned flags, result_amount_type rt ) override
  {
    // Exsanguinated bleeds snapshot hasted tick time when the ticks are rescheduled.
    // This will make snapshot_internal on haste updates discard the new value.
    if ( cast_state( state )->is_exsanguinated() )
    {
      flags &= ~STATE_HASTE;
    }
    
    ab::update_state( state, flags, rt );
  }

  void snapshot_state( action_state_t* state, result_amount_type rt ) override
  {
    int consume_cp = as<int>( std::min( p()->current_cp(), p()->consume_cp_max() ) );
    int effective_cp = consume_cp;

    // Apply and Snapshot Echoing Reprimand Buffs
    if ( p()->spell.echoing_reprimand->ok() && consumes_echoing_reprimand() )
    {
      if ( range::any_of( p()->buffs.echoing_reprimand, [ consume_cp ]( const buff_t* buff ) { return buff->check_value() == consume_cp; } ) )
        effective_cp = as<int>( p()->spell.echoing_reprimand->effectN( 2 ).base_value() );
    }

    auto rs = cast_state( state );
    rs->set_combo_points( consume_cp, effective_cp );
    rs->clear_exsanguinated();

    ab::snapshot_state( state, rt );
  }

  // Secondary Trigger Functions ==============================================

  bool is_secondary_action() const
  { return secondary_trigger_type != secondary_trigger::NONE && ab::background == true; }

  virtual void trigger_secondary_action( player_t* target, int cp = 0, timespan_t delay = timespan_t::zero() )
  {
    assert( is_secondary_action() );
    make_event<secondary_action_trigger_t<base_t>>( *ab::sim, target, this, cp, delay );
  }

  virtual void trigger_secondary_action( player_t* target, timespan_t delay )
  {
    trigger_secondary_action( target, 0, delay );
  }

  virtual void trigger_secondary_action( action_state_t* s, timespan_t delay = timespan_t::zero() )
  {
    assert( is_secondary_action() && s->action == this );
    make_event<secondary_action_trigger_t<base_t>>( *ab::sim, s, delay );
  }

  // Helper Functions =========================================================

  // Helper function for expressions. Returns the number of guaranteed generated combo points for
  // this ability, taking into account any potential buffs.
  virtual double generate_cp() const
  {
    double cp = 0;

    if ( ab::energize_type != action_energize::NONE && ab::energize_resource == RESOURCE_COMBO_POINT )
    {
      cp += ab::energize_amount;

      if ( affected_by.broadside_cp )
      {
        cp += p()->buffs.broadside->check_value();
      }

      if ( affected_by.shadow_blades_cp && p()->buffs.shadow_blades->check() )
      {
        cp += p()->buffs.shadow_blades->data().effectN( 2 ).base_value();
      }

      if ( affected_by.improved_ambush )
      {
        cp += p()->talent.rogue.improved_ambush->effectN( 1 ).base_value();
      }
    }

    return cp;
  }

  virtual bool procs_poison() const
  { return ab::weapon != nullptr && ab::has_amount_result(); }

  // 2022-10-23 -- As of the latest beta build it appears all expected abilities proc Deadly Poison
  virtual bool procs_deadly_poison() const
  { return procs_poison(); }

  // Generic rules for proccing Main Gauche, used by rogue_t::trigger_main_gauche()
  virtual bool procs_main_gauche() const
  { return false; }

  // Generic rules for proccing Fatal Flourish, used by rogue_t::trigger_fatal_flourish()
  virtual bool procs_fatal_flourish() const
  { return ab::callbacks && !ab::proc && ab::weapon != nullptr && ab::weapon->slot == SLOT_OFF_HAND; }

  // Generic rules for proccing Blade Flurry, used by rogue_t::trigger_blade_flurry()
  virtual bool procs_blade_flurry() const
  { return false; }

  // Generic rules for proccing Shadow Blades, used by rogue_t::trigger_shadow_blades_attack()
  virtual bool procs_shadow_blades_damage() const
  { return ab::energize_type != action_energize::NONE && ab::energize_amount > 0 && ab::energize_resource == RESOURCE_COMBO_POINT; }

  // Generic rules for proccing Banshee's Blight, used by rogue_t::trigger_banshees_blight()
  virtual bool procs_banshees_blight() const
  { return ab::base_costs[ RESOURCE_COMBO_POINT ] > 0 && ( ab::attack_power_mod.direct > 0.0 || ab::attack_power_mod.tick > 0.0 ); }

  // Generic rules for snapshotting the Nightstalker pmultiplier, default to DoTs only.
  // If a DoT with DD component is whitelisted in the direct damage effect #1 on 130493 this can double dip.
  virtual bool snapshots_nightstalker() const
  { return ab::dot_duration > timespan_t::zero() && ab::base_tick_time > timespan_t::zero(); }

  // Overridable wrapper for checking stealth requirement
  virtual bool requires_stealth() const
  {
    if ( affected_by.audacity && p()->buffs.audacity->check() )
      return false;

    if ( affected_by.blindside && p()->buffs.blindside->check() )
      return false;

    if ( affected_by.sepsis && p()->buffs.sepsis->check() )
      return false;

    return _requires_stealth;
  }

  // Overridable wrapper for checking stealth breaking
  virtual bool breaks_stealth() const
  { return _breaks_stealth; }

  // Overridable function to determine whether a finisher is working with Echoing Reprimand.
  virtual bool consumes_echoing_reprimand() const
  { return ab::base_costs[ RESOURCE_COMBO_POINT ] > 0 && ( ab::attack_power_mod.direct > 0.0 || ab::attack_power_mod.tick > 0.0 ); }

public:
  // Ability triggers
  void spend_combo_points( const action_state_t* );
  void trigger_auto_attack( const action_state_t* );
  void trigger_poisons( const action_state_t* );
  void trigger_seal_fate( const action_state_t* );
  void trigger_main_gauche( const action_state_t* );
  void trigger_fatal_flourish( const action_state_t* );
  void trigger_energy_refund();
  void trigger_poison_bomb( const action_state_t* );
  void trigger_venomous_wounds( const action_state_t* );
  void trigger_blade_flurry( const action_state_t* );
  void trigger_ruthlessness_cp( const action_state_t* );
  void trigger_combo_point_gain( int, gain_t* gain = nullptr, bool requires_reaction = false );
  void trigger_elaborate_planning( const action_state_t* );
  void trigger_alacrity( const action_state_t* );
  void trigger_deepening_shadows( const action_state_t* );
  void trigger_shadow_techniques( const action_state_t* );
  void trigger_weaponmaster( const action_state_t*, rogue_attack_t* action );
  void trigger_opportunity( const action_state_t*, rogue_attack_t* action );
  void trigger_restless_blades( const action_state_t* );
  void trigger_dreadblades( const action_state_t* );
  void trigger_relentless_strikes( const action_state_t* );
  void trigger_venom_rush( const action_state_t* );
  void trigger_blindside( const action_state_t* );
  void trigger_shadow_blades_attack( action_state_t* );
  void trigger_prey_on_the_weak( const action_state_t* state );
  void trigger_find_weakness( const action_state_t* state, timespan_t duration = timespan_t::min() );
  void trigger_grand_melee( const action_state_t* state );
  void trigger_master_of_shadows();
  void trigger_akaaris_soul_fragment( const action_state_t* state );
  void trigger_bloodfang( const action_state_t* state );
  void trigger_guile_charm( const action_state_t* state );
  void trigger_dashing_scoundrel( const action_state_t* state );
  void trigger_count_the_odds( const action_state_t* state );
  void trigger_keep_it_rolling();
  void trigger_flagellation( const action_state_t* state );
  void trigger_perforated_veins( const action_state_t* state );
  void trigger_banshees_blight( const action_state_t* state );
  void trigger_lingering_shadow( const action_state_t* state );
  void trigger_danse_macabre( const action_state_t* state );

  // General Methods ==========================================================

  void update_ready( timespan_t cd_duration = timespan_t::min() ) override
  {
    if ( secondary_trigger_type != secondary_trigger::NONE )
    {
      cd_duration = timespan_t::zero();
    }

    ab::update_ready( cd_duration );
  }

  timespan_t gcd() const override
  {
    timespan_t t = ab::gcd();

    if ( affected_by.adrenaline_rush_gcd && t != timespan_t::zero() && p()->buffs.adrenaline_rush->check() )
    {
      t += p()->buffs.adrenaline_rush->data().effectN( 3 ).time_value();
    }

    return t;
  }

  virtual double combo_point_da_multiplier( const action_state_t* s ) const
  {
    if ( ab::base_costs[ RESOURCE_COMBO_POINT ] )
      return static_cast<double>( cast_state( s )->get_combo_points() );

    return 1.0;
  }

  double composite_da_multiplier( const action_state_t* state ) const override
  {
    double m = ab::composite_da_multiplier( state );

    m *= combo_point_da_multiplier( state );

    // Registered Damage Buffs
    for ( auto damage_buff : direct_damage_buffs )
      m *= damage_buff->stack_value_direct();
    
    // Mastery
    if ( affected_by.mastery_executioner.direct || affected_by.mastery_potent_assassin.direct )
    {
      m *= 1.0 + p()->cache.mastery_value();
    }

    // Apply Nightstalker direct damage increase via the corresponding driver spell.
    // And yes, this can cause double dips with the persistent multiplier on DoTs which was the case with Crimson Tempest once.
    if ( affected_by.nightstalker && p()->stealthed( STEALTH_BASIC | STEALTH_SHADOWDANCE ) )
    {
      m *= p()->buffs.nightstalker->direct_mod.multiplier;
    }

    if ( affected_by.t29_assassination_2pc && p()->buffs.envenom->check() )
    {
      m *= 1.0 + p()->set_bonuses.t29_assassination_2pc->effectN( 1 ).percent();
    }

    return m;
  }

  double composite_ta_multiplier( const action_state_t* state ) const override
  {
    double m = ab::composite_ta_multiplier( state );

    // Registered Damage Buffs
    for ( auto damage_buff : periodic_damage_buffs )
      m *= damage_buff->stack_value_periodic();

    // Masteries for Assassination and Subtlety have periodic damage in a separate effect. Just to be sure, use that instead of direct mastery_value.
    if ( affected_by.mastery_executioner.periodic )
    {
      m *= 1.0 + p()->cache.mastery() * p()->mastery.executioner->effectN( 2 ).mastery_value();
    }

    if ( affected_by.mastery_potent_assassin.periodic )
    {
      m *= 1.0 + p()->cache.mastery() * p()->mastery.potent_assassin->effectN( 2 ).mastery_value();
    }

    // DFALPHA TOCHECK -- Currently does nothing even after the pmultiplier removal
    // Apply Nightstalker periodic damage increase via the corresponding driver spell.
    //if ( affected_by.nightstalker && p()->stealthed( STEALTH_BASIC | STEALTH_SHADOWDANCE ) )
    //{
    //  m *= p()->buffs.nightstalker->periodic_mod.multiplier;
    //}

    return m;
  }

  double composite_target_multiplier( player_t* target ) const override
  {
    double m = ab::composite_target_multiplier( target );

    auto tdata = td( target );

    if ( affected_by.improved_shiv )
    {
      m *= tdata->debuffs.shiv->value_direct();
    }

    if ( affected_by.zoldyck_insignia &&
         target->health_percentage() < p()->spec.zoldyck_insignia->effectN( 2 ).base_value() )
    {
      m *= 1.0 + p()->spec.zoldyck_insignia->effectN( 1 ).percent();
    }

    if ( affected_by.maim_mangle && tdata->dots.garrote->is_ticking() )
    {
      m *= 1.0 + ( p()->talent.assassination.systemic_failure->ok() ?
                   p()->talent.assassination.systemic_failure->effectN( 1 ).percent() :
                   p()->conduit.maim_mangle.percent() );
    }

    if ( affected_by.lethal_dose )
    {
      int lethal_dose_count = tdata->total_bleeds() + tdata->total_poisons();
      // DFALPHA TOCHECK -- What does and doesn't trigger this for retail?
      if ( p()->bugs )
      {
        lethal_dose_count -= ( tdata->dots.serrated_bone_spike->is_ticking() +
                               tdata->dots.crimson_tempest->is_ticking() );
      }
      m *= 1.0 + ( p()->talent.assassination.lethal_dose->effectN( 1 ).percent() * lethal_dose_count );
    }

    if ( affected_by.deathmark )
    {
      m *= tdata->debuffs.deathmark->value_direct();
    }

    return m;
  }

  double composite_persistent_multiplier( const action_state_t* state ) const override
  {
    double m = ab::composite_persistent_multiplier( state );

    // DFALPHA TOCHECK -- Persistent multipliers are currently disabled as of the latest build
    // Apply Nightstalker as a Persistent Multiplier for things that snapshot
    // This appears to be driven by the dummy effect #2 and there is no whitelist.
    // This can and will cause double dips on direct damage if a spell is whitelisted in effect #1.
    //if ( p()->talent.rogue.nightstalker->ok() && snapshots_nightstalker() && p()->stealthed( STEALTH_BASIC | STEALTH_SHADOWDANCE ) )
    //{
    //  m *= p()->buffs.nightstalker->periodic_mod.multiplier;
    //}

    return m;
  }

  double composite_crit_chance() const override
  {
    double c = ab::composite_crit_chance();

    // Registered Damage Buffs
    for ( auto crit_chance_buff : crit_chance_buffs )
      c += crit_chance_buff->stack_value_crit_chance();

    if ( affected_by.dashing_scoundrel && p()->buffs.envenom->check() )
    {
      c += p()->spec.dashing_scoundrel->effectN( 1 ).percent();
    }

    return c;
  }

  timespan_t tick_time( const action_state_t* state ) const override
  {
    timespan_t tt = ab::tick_time( state );

    tt *= 1.0 / cast_state( state )->get_exsanguinated_rate();

    return tt;
  }

  virtual double composite_poison_flat_modifier( const action_state_t* ) const
  { return 0.0; }

  double cost() const override
  {
    double c = ab::cost();

    if ( c <= 0 )
      return 0;

    if ( p()->talent.subtlety.shadow_focus->ok() && p()->stealthed( STEALTH_BASIC | STEALTH_SHADOWDANCE ) )
    {
      c *= 1.0 + p()->spec.shadow_focus_buff->effectN( 1 ).percent();
    }

    if ( affected_by.blindside )
    {
      c *= 1.0 + p()->buffs.blindside->check_value();
    }

    if ( c <= 0 )
      c = 0;

    return c;
  }

  void consume_resource() override
  {
    // Abilities triggered as part of another ability (secondary triggers) do not consume resources
    if ( is_secondary_action() )
    {
      return;
    }

    ab::consume_resource();

    spend_combo_points( ab::execute_state );

    if ( ab::current_resource() == RESOURCE_ENERGY && ab::last_resource_cost > 0 )
    {
      if ( !ab::hit_any_target )
      {
        trigger_energy_refund();
      }
      else
      {
        // Duskwalker's Patch Legendary
        if ( p()->legendary.duskwalkers_patch.ok() )
        {
          p()->legendary.duskwalkers_patch_counter += ab::last_resource_cost;
          while ( p()->legendary.duskwalkers_patch_counter >= p()->legendary.duskwalkers_patch->effectN( 2 ).base_value() )
          {
            // TOCHECK -- Have been informed this is likely to work on Deathmark in prepatch
            p()->cooldowns.deathmark->adjust( -timespan_t::from_seconds( p()->legendary.duskwalkers_patch->effectN( 1 ).base_value() ) );
            p()->legendary.duskwalkers_patch_counter -= p()->legendary.duskwalkers_patch->effectN( 2 ).base_value();
            p()->procs.duskwalker_patch->occur();
          }
        }
      }
    }
  }

  void execute() override
  {
    // 2020-12-04- Hotfix notes this is no longer consumed "while under the effects Stealth, Vanish, Subterfuge, Shadow Dance, and Shadowmeld"
    // 2021-04-22 - Night Fae Lego Toxic Onslaught on PTR shows this happens and applies proc buffs before damage (Shadow Blades)
    if ( affected_by.sepsis && p()->buffs.sepsis->check() && !p()->stealthed( STEALTH_ALL & ~STEALTH_SEPSIS ) )
    {
      p()->buffs.sepsis->decrement();
    }

    ab::execute();

    if ( ab::harmful )
      p()->restealth_allowed = false;

    if ( ab::hit_any_target )
    {
      trigger_auto_attack( ab::execute_state );
      trigger_ruthlessness_cp( ab::execute_state );

      if ( ab::energize_type != action_energize::NONE && ab::energize_resource == RESOURCE_COMBO_POINT )
      {
        if ( affected_by.shadow_blades_cp && p()->buffs.shadow_blades->up() )
        {
          trigger_combo_point_gain( as<int>( p()->buffs.shadow_blades->data().effectN( 2 ).base_value() ), p()->gains.shadow_blades );
        }

        if ( affected_by.broadside_cp && p()->buffs.broadside->up() )
        {
          trigger_combo_point_gain( as<int>( p()->buffs.broadside->check_value() ), p()->gains.broadside );
        }

        if ( affected_by.improved_ambush )
        {
          trigger_combo_point_gain( as<int>( p()->talent.rogue.improved_ambush->effectN( 1 ).base_value() ), p()->gains.improved_ambush );
        }
      }

      trigger_danse_macabre( ab::execute_state );
      trigger_dreadblades( ab::execute_state );
      trigger_relentless_strikes( ab::execute_state );
      trigger_elaborate_planning( ab::execute_state );
      trigger_alacrity( ab::execute_state );
      trigger_deepening_shadows( ab::execute_state );
      trigger_flagellation( ab::execute_state );
      trigger_banshees_blight( ab::execute_state );
    }

    // Trigger the 1ms delayed breaking of all stealth buffs
    if ( p()->stealthed( STEALTH_BASIC | STEALTH_SHADOWMELD ) && breaks_stealth() )
    {
      p()->break_stealth();
    }

    // Expire On-Cast Fading Buffs
    for ( auto consume_buff : consume_buffs )
    {
      consume_buff.first->expire();
      if ( consume_buff.second )
        consume_buff.second->occur();
    }

    // Debugging
    if ( p()->sim->log )
    {
      const auto rs = cast_state( ab::execute_state );
      if ( rs->get_exsanguinated_rate() != 1.0 )
      {
        p()->sim->print_log( "{} casts {} with initial exsanguinated rate of {:.1f}", *p(), *this, rs->get_exsanguinated_rate() );
      }
    }
  }

  void schedule_travel( action_state_t* state ) override
  {
    ab::schedule_travel( state );

    if ( ab::energize_type != action_energize::NONE && ab::energize_resource == RESOURCE_COMBO_POINT )
    {
      trigger_seal_fate( state );
    }

    trigger_poisons( state );
  }

  bool ready() override
  {
    if ( !ab::ready() )
      return false;

    if ( ab::base_costs[ RESOURCE_COMBO_POINT ] > 0 &&
      p()->current_cp() < ab::base_costs[ RESOURCE_COMBO_POINT ] )
      return false;

    if ( requires_stealth() && !p()->stealthed() )
    {
      return false;
    }

    return true;
  }

  std::unique_ptr<expr_t> create_expression( util::string_view name_str ) override;
};

// ==========================================================================
// Rogue Attack Classes
// ==========================================================================

struct rogue_heal_t : public rogue_action_t<heal_t>
{
  rogue_heal_t( util::string_view n, rogue_t* p,
                       const spell_data_t* s = spell_data_t::nil(),
                       util::string_view o = {} )
    : base_t( n, p, s, o )
  {
    harmful = false;
    set_target( p );
  }

  bool breaks_stealth() const override
  { return false; }
};

struct rogue_spell_t : public rogue_action_t<spell_t>
{
  rogue_spell_t( util::string_view n, rogue_t* p,
                        const spell_data_t* s = spell_data_t::nil(),
                        util::string_view o = {} )
    : base_t( n, p, s, o )
  {}
};

struct rogue_attack_t : public rogue_action_t<melee_attack_t>
{
  rogue_attack_t( util::string_view n, rogue_t* p,
                         const spell_data_t* s = spell_data_t::nil(),
                         util::string_view o = {} )
    : base_t( n, p, s, o )
  {
    special = true;
  }

  void impact( action_state_t* state ) override
  {
    base_t::impact( state );

    trigger_main_gauche( state );
    trigger_fatal_flourish( state );
    trigger_blade_flurry( state );
    trigger_shadow_blades_attack( state );
    trigger_bloodfang( state );
    trigger_dashing_scoundrel( state );
  }

  void tick( dot_t* d ) override
  {
    base_t::tick( d );

    trigger_shadow_blades_attack( d->state );
    trigger_dashing_scoundrel( d->state );
  }
};

// ==========================================================================
// Poisons
// ==========================================================================

struct rogue_poison_t : public rogue_attack_t
{
  bool is_lethal;
  double base_proc_chance;
  rogue_attack_t* deathmark_impact_action;

  rogue_poison_t( util::string_view name, rogue_t* p, const spell_data_t* s,
                  bool is_lethal = false, bool triggers_procs = false ) :
    actions::rogue_attack_t( name, p, s ),
    is_lethal( is_lethal ),
    base_proc_chance( 0.0 ),
    deathmark_impact_action( nullptr )
  {
    background = dual = true;
    proc = !triggers_procs;
    callbacks = triggers_procs;

    trigger_gcd = timespan_t::zero();

    base_proc_chance = data().proc_chance();
    if ( s->affected_by( p->talent.assassination.improved_poisons->effectN( 1 ) ) )
    {
      base_proc_chance += p->talent.assassination.improved_poisons->effectN( 1 ).percent();
    }
  }

  timespan_t execute_time() const override
  {
    return timespan_t::zero();
  }

  virtual double proc_chance( const action_state_t* source_state ) const
  {
    if ( base_proc_chance == 0.0 )
      return 0.0;

    double chance = base_proc_chance;
    chance += p()->buffs.envenom->stack_value();
    chance += rogue_t::cast_attack( source_state->action )->composite_poison_flat_modifier( source_state );

    return chance;
  }

  virtual void trigger( const action_state_t* source_state )
  {
    bool result = rng().roll( proc_chance( source_state ) );

    sim->print_debug( "{} attempts to proc {}, target={} source={} proc_chance={}: {}", *player, *this,
                      *source_state->target, *source_state->action, proc_chance( source_state ), result );

    if ( !result )
      return;

    set_target( source_state->target );
    execute();
  }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );

    if ( is_lethal && state->result_amount > 0 )
    {
      if ( td( state->target )->dots.kingsbane->is_ticking() )
        p()->buffs.kingsbane->trigger();

      if ( p()->set_bonuses.t29_assassination_4pc->ok() )
        p()->buffs.t29_assassination_4pc->trigger();
    }

    if ( deathmark_impact_action && td( state->target )->dots.deathmark->is_ticking() )
    {
      deathmark_impact_action->trigger_secondary_action( state->target );
    }
  }

};

// Deadly Poison ============================================================

struct deadly_poison_t : public rogue_poison_t
{
  struct deadly_poison_dd_t : public rogue_poison_t
  {
    deadly_poison_dd_t( util::string_view name, rogue_t* p, const spell_data_t* s ) :
      rogue_poison_t( name, p, s, true, true )
    {
    }
  };

  struct deadly_poison_dot_t : public rogue_poison_t
  {
    deadly_poison_dot_t( util::string_view name, rogue_t* p, const spell_data_t* s ) :
      rogue_poison_t( name, p, s, true, true )
    {
    }
  };

  deadly_poison_dd_t* proc_instant;
  deadly_poison_dot_t* proc_dot;

  deadly_poison_t( util::string_view name, rogue_t* p ) :
    rogue_poison_t( name, p, p->talent.assassination.deadly_poison ),
    proc_instant( nullptr ), proc_dot( nullptr )
  {
    proc_instant = p->get_background_action<deadly_poison_dd_t>(
      "deadly_poison_instant", p->spec.deadly_poison_instant );
    proc_dot = p->get_background_action<deadly_poison_dot_t>(
      "deadly_poison_dot", p->talent.assassination.deadly_poison->effectN( 1 ).trigger() );

    add_child( proc_instant );
    add_child( proc_dot );
  }

  void impact( action_state_t* state ) override
  {
    rogue_poison_t::impact( state );

    rogue_td_t* tdata = td( state->target );
    if ( tdata->dots.deadly_poison->is_ticking() )
    {
      proc_instant->set_target( state->target );
      proc_instant->execute();
    }
    proc_dot->set_target( state->target );
    proc_dot->execute();

    if ( tdata->dots.deathmark->is_ticking() )
    {
      if ( tdata->dots.deadly_poison_deathmark->is_ticking() )
      {
        p()->active.deathmark.deadly_poison_instant->trigger_secondary_action( state->target );
      }
      p()->active.deathmark.deadly_poison_dot->trigger_secondary_action( state->target );
    }
  }
};

// Instant Poison ===========================================================

struct instant_poison_t : public rogue_poison_t
{
  struct instant_poison_dd_t : public rogue_poison_t
  {
    instant_poison_dd_t( util::string_view name, rogue_t* p, const spell_data_t* s ) :
      rogue_poison_t( name, p, s, true, true )
    {
    }

    double composite_da_multiplier( const action_state_t* state ) const override
    {
      double m = rogue_poison_t::composite_da_multiplier( state );

      // 2020-10-18- Nightstalker appears to buff Instant Poison by the base 50% amount, despite being in no whitelists
      if ( p()->bugs && p()->talent.rogue.nightstalker->ok() && p()->stealthed( STEALTH_BASIC | STEALTH_SHADOWDANCE ) )
      {
        m *= 1.0 + p()->spell.nightstalker_buff->effectN( 2 ).percent();
      }

      return m;
    }
  };

  instant_poison_t( util::string_view name, rogue_t* p ) :
    rogue_poison_t( name, p, p->spell.instant_poison )
  {
    impact_action = p->get_background_action<instant_poison_dd_t>(
      "instant_poison", p->spell.instant_poison->effectN( 1 ).trigger() );

    if ( p->talent.assassination.deathmark->ok() )
    {
      deathmark_impact_action = p->active.deathmark.instant_poison;
    }
  }
};

// Wound Poison =============================================================

struct wound_poison_t : public rogue_poison_t
{
  struct wound_poison_dd_t : public rogue_poison_t
  {
    wound_poison_dd_t( util::string_view name, rogue_t* p, const spell_data_t* s ) :
      rogue_poison_t( name, p, s, true, true )
    {
    }

    void impact( action_state_t* state ) override
    {
      rogue_poison_t::impact( state );
      td( state->target )->debuffs.wound_poison->trigger();
      if ( !sim->overrides.mortal_wounds && state->target->debuffs.mortal_wounds )
      {
        state->target->debuffs.mortal_wounds->trigger( data().duration() );
      }
    }
  };

  wound_poison_t( util::string_view name, rogue_t* p ) :
    rogue_poison_t( name, p, p->spell.wound_poison )
  {
    impact_action = p->get_background_action<wound_poison_dd_t>(
      "wound_poison", p->spell.wound_poison->effectN( 1 ).trigger() );

    if ( p->talent.assassination.deathmark->ok() )
    {
      deathmark_impact_action = p->active.deathmark.wound_poison;
    }
  }
};

// Amplifying Poison ========================================================

struct amplifying_poison_t : public rogue_poison_t
{
  struct amplifying_poison_dd_t : public rogue_poison_t
  {
    amplifying_poison_dd_t( util::string_view name, rogue_t* p, const spell_data_t* s ) :
      rogue_poison_t( name, p, s, true, true )
    {
    }

    void impact( action_state_t* state ) override
    {
      rogue_poison_t::impact( state );

      if ( secondary_trigger_type == secondary_trigger::DEATHMARK )
      {
        td( state->target )->debuffs.amplifying_poison_deathmark->trigger();
      }
      else
      {
        td( state->target )->debuffs.amplifying_poison->trigger();
      }
    }
  };

  amplifying_poison_t( util::string_view name, rogue_t* p ) :
    rogue_poison_t( name, p, p->talent.assassination.amplifying_poison )
  {
    impact_action = p->get_background_action<amplifying_poison_dd_t>(
      "amplifying_poison", p->talent.assassination.amplifying_poison->effectN( 3 ).trigger() );

    if ( p->talent.assassination.deathmark->ok() )
    {
      deathmark_impact_action = p->active.deathmark.amplifying_poison;
    }
  }
};

// Atrophic Poison ==========================================================

struct atrophic_poison_t : public rogue_poison_t
{
  struct atrophic_poison_proc_t : public rogue_poison_t
  {
    atrophic_poison_proc_t( util::string_view name, rogue_t* p ) :
      rogue_poison_t( name, p, p->talent.rogue.atrophic_poison->effectN( 1 ).trigger(), false, true )
    {
    }

    void impact( action_state_t* state ) override
    {
      rogue_poison_t::impact( state );
      td( state->target )->debuffs.atrophic_poison->trigger();
    }
  };

  atrophic_poison_t( util::string_view name, rogue_t* p ) :
    rogue_poison_t( name, p, p->talent.rogue.atrophic_poison )
  {
    impact_action = p->get_background_action<atrophic_poison_proc_t>( "atrophic_poison" );
  }
};

// Crippling Poison =========================================================

struct crippling_poison_t : public rogue_poison_t
{
  struct crippling_poison_proc_t : public rogue_poison_t
  {
    crippling_poison_proc_t( util::string_view name, rogue_t* p ) :
      rogue_poison_t( name, p, p->spell.crippling_poison->effectN( 1 ).trigger(), false, true )
    { }

    void impact( action_state_t* state ) override
    {
      rogue_poison_t::impact( state );
      td( state->target )->debuffs.crippling_poison->trigger();
    }
  };

  crippling_poison_t( util::string_view name, rogue_t* p ) :
    rogue_poison_t( name, p, p->spell.crippling_poison )
  {
    impact_action = p->get_background_action<crippling_poison_proc_t>( "crippling_poison" );
  }
};

// Numbing Poison ===========================================================

struct numbing_poison_t : public rogue_poison_t
{
  struct numbing_poison_proc_t : public rogue_poison_t
  {
    numbing_poison_proc_t( util::string_view name, rogue_t* p ) :
      rogue_poison_t( name, p, p->talent.rogue.numbing_poison->effectN( 1 ).trigger() )
    { }

    void impact( action_state_t* state ) override
    {
      rogue_poison_t::impact( state );
      td( state->target )->debuffs.numbing_poison->trigger();
    }
  };

  numbing_poison_t( util::string_view name, rogue_t* p ) :
    rogue_poison_t( name, p, p->talent.rogue.numbing_poison )
  {
    impact_action = p->get_background_action<numbing_poison_proc_t>( "numbing_poison" );
  }
};

// Apply Poison =============================================================

struct apply_poison_t : public action_t
{
  bool executed;

  apply_poison_t( rogue_t* p, util::string_view options_str ) :
    action_t( ACTION_OTHER, "apply_poison", p ),
    executed( false )
  {
    std::string lethal_str;
    std::string lethal_dtb_str;
    std::string nonlethal_str;
    std::string nonlethal_dtb_str;

    add_option( opt_string( "lethal", lethal_str ) );
    add_option( opt_string( "lethal_dtb", lethal_dtb_str ) );
    add_option( opt_string( "nonlethal", nonlethal_str ) );
    add_option( opt_string( "nonlethal_dtb", nonlethal_dtb_str ) );
    parse_options( options_str );

    trigger_gcd = timespan_t::zero();
    harmful = false;

    if ( p->main_hand_weapon.type != WEAPON_NONE || p->off_hand_weapon.type != WEAPON_NONE )
    {
      if ( !p->active.lethal_poison )
      {
        if ( lethal_str.empty() )
        {
          lethal_str = get_default_lethal_poison( p );
        }
        
        p->active.lethal_poison = get_poison( p, lethal_str );
        sim->print_log( "{} applies lethal poison {}", *p, *p->active.lethal_poison );
      }

      if ( !p->active.nonlethal_poison )
      {
        if ( nonlethal_str.empty() )
        {
          nonlethal_str = get_default_nonlethal_poison_dtb( p );;
        }

        p->active.nonlethal_poison = get_poison( p, nonlethal_str );
        sim->print_log( "{} applies non-lethal poison {}", *p, *p->active.nonlethal_poison );
      }

      if ( p->talent.assassination.dragon_tempered_blades->ok() )
      {
        if ( !p->active.lethal_poison_dtb )
        {
          if ( lethal_dtb_str.empty() || get_poison( p, lethal_dtb_str ) == p->active.lethal_poison )
          {
            lethal_dtb_str = get_default_lethal_poison_dtb( p );
          }

          p->active.lethal_poison_dtb = get_poison( p, lethal_dtb_str );
          sim->print_log( "{} applies second lethal poison {}", *p, *p->active.lethal_poison_dtb );

          if ( nonlethal_dtb_str.empty() || get_poison( p, nonlethal_dtb_str ) == p->active.nonlethal_poison )
          {
            nonlethal_dtb_str = get_default_nonlethal_poison_dtb( p );
          }

          p->active.nonlethal_poison_dtb = get_poison( p, nonlethal_dtb_str );
          sim->print_log( "{} applies second non-lethal poison {}", *p, *p->active.nonlethal_poison_dtb );
        }
      }
    }
  }

  std::string get_default_lethal_poison( rogue_t* p )
  {
    if ( p->talent.assassination.amplifying_poison->ok() && !p->talent.assassination.dragon_tempered_blades->ok() )
      return "amplifying";

    if ( p->talent.assassination.deadly_poison->ok() )
      return "deadly";

    return "instant";
  }

  std::string get_default_lethal_poison_dtb( rogue_t* p )
  {
    if ( p->active.lethal_poison && p->active.lethal_poison->data().id() == p->talent.assassination.deadly_poison->id() )
    {
      return p->talent.assassination.amplifying_poison->ok() ? "amplifying" : "instant";
    }
    else
    {
      return "deadly";
    }
  }

  std::string get_default_nonlethal_poison( rogue_t* p )
  {
    return "crippling";
  }

  std::string get_default_nonlethal_poison_dtb( rogue_t* p )
  {
    if ( p->active.nonlethal_poison && p->active.nonlethal_poison->data().id() == p->spell.crippling_poison->id() )
    {
      if ( p->talent.rogue.numbing_poison->ok() )
        return "numbing";
      else if ( p->talent.rogue.atrophic_poison->ok() )
        return "atrophic";
      
      return "none";
    }
    else
    {
      return "crippling";
    }
  }

  rogue_poison_t* get_poison( rogue_t* p, util::string_view poison_name )
  {
    if ( poison_name == "none" || poison_name.empty() )
      return nullptr;
    else if ( poison_name == "deadly" )
      return p->get_background_action<deadly_poison_t>( "deadly_poison_driver" );
    else if ( poison_name == "instant" )
      return p->get_background_action<instant_poison_t>( "instant_poison_driver" );
    else if ( poison_name == "amplifying" )
      return p->get_background_action<amplifying_poison_t>( "amplifying_poison_driver" );
    else if ( poison_name == "wound" )
      return p->get_background_action<wound_poison_t>( "wound_poison_driver" );
    else if ( poison_name == "atrophic" )
      return p->get_background_action<atrophic_poison_t>( "atrophic_poison_driver" );
    else if ( poison_name == "crippling" )
      return p->get_background_action<crippling_poison_t>( "crippling_poison_driver" );
    else if ( poison_name == "numbing" )
      return p->get_background_action<numbing_poison_t>( "numbing_poison_driver" );

    sim->error( "Player {} attempting to apply unknown poison {}.", *p, poison_name );
    return nullptr;
  }

  void reset() override
  {
    action_t::reset();
    executed = false;
  }

  void execute() override
  {
    executed = true;
    sim->print_log( "{} performs {}", *player, *this );
  }

  bool ready() override
  {
    return !executed;
  }
};

// ==========================================================================
// Attacks
// ==========================================================================

// Melee Attack =============================================================

struct melee_t : public rogue_attack_t
{
  int sync_weapons;
  bool first;
  bool canceled;
  timespan_t prev_scheduled_time;

  melee_t( const char* name, rogue_t* p, int sw ) :
    rogue_attack_t( name, p ), sync_weapons( sw ), first( true ),
    canceled( false ), prev_scheduled_time( timespan_t::zero() )
  {
    background = repeating = may_glance = may_crit = true;
    allow_class_ability_procs = not_a_proc = true;
    special = false;
    school = SCHOOL_PHYSICAL;
    trigger_gcd = timespan_t::zero();
    weapon_multiplier = 1.0;

    if ( p->dual_wield() )
      base_hit -= 0.19;
  }

  void reset() override
  {
    rogue_attack_t::reset();
    first = true;
    canceled = false;
    prev_scheduled_time = timespan_t::zero();
  }

  timespan_t execute_time() const override
  {
    timespan_t t = rogue_attack_t::execute_time();
    
    if ( first )
    {
      return ( weapon->slot == SLOT_OFF_HAND ) ? ( sync_weapons ? timespan_t::zero() : t / 2 ) : timespan_t::zero();
    }

    // If we cancel the swing timer mid-fight, use the previous swing timer
    if ( canceled )
    {
      return std::min( t, std::max( prev_scheduled_time - p()->sim->current_time(), timespan_t::zero() ) );
    }

    return t;
  }

  void schedule_execute( action_state_t* state ) override
  {
    rogue_attack_t::schedule_execute( state );

    if ( first )
    {
      first = false;
      p()->sim->print_log( "{} schedules AA start {} with {} swing timer", *p(), *this, time_to_execute );
    }

    if ( canceled )
    {
      canceled = false;
      prev_scheduled_time = timespan_t::zero();
      p()->sim->print_log( "{} schedules AA restart {} with {} swing timer remaining", *p(), *this, time_to_execute );
    }
  }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );
    trigger_shadow_techniques( state );
  }

  double composite_target_multiplier( player_t* target ) const override
  {
    double m = rogue_attack_t::composite_target_multiplier( target );

    return m;
  }

  double composite_crit_chance() const override
  {
    double c = rogue_attack_t::composite_crit_chance();

    if ( p()->talent.assassination.master_assassin->ok() )
    {
      c += p()->buffs.master_assassin->stack_value();
      c += p()->buffs.master_assassin_aura->stack_value();
    }

    if ( p()->legendary.master_assassins_mark->ok() )
    {
      c += p()->buffs.master_assassins_mark->stack_value();
      c += p()->buffs.master_assassins_mark_aura->stack_value();
    }

    return c;
  }

  double action_multiplier() const override
  {
    double m = rogue_attack_t::action_multiplier();

    // Registered Damage Buffs
    for ( auto damage_buff : auto_attack_damage_buffs )
      m *= damage_buff->stack_value_auto_attack();

    return m;
  }

  bool procs_poison() const override
  { return true; }

  bool procs_deadly_poison() const override
  { return true; }

  bool procs_main_gauche() const override
  { return weapon->slot == SLOT_MAIN_HAND; }

  bool procs_blade_flurry() const override
  { return true; }
};

// Auto Attack ==============================================================

struct auto_melee_attack_t : public action_t
{
  int sync_weapons;

  auto_melee_attack_t( rogue_t* p, util::string_view options_str ) :
    action_t( ACTION_OTHER, "auto_attack", p ),
    sync_weapons( 0 )
  {
    trigger_gcd = timespan_t::zero();

    add_option( opt_bool( "sync_weapons", sync_weapons ) );
    parse_options( options_str );

    if ( p -> main_hand_weapon.type == WEAPON_NONE )
    {
      background = true;
      return;
    }

    p->melee_main_hand = debug_cast<melee_t*>( p->find_action( "auto_attack_mh" ) );
    if ( !p->melee_main_hand )
      p->melee_main_hand = new melee_t( "auto_attack_mh", p, sync_weapons );

    p->main_hand_attack = p->melee_main_hand;
    p->main_hand_attack->weapon = &( p->main_hand_weapon );
    p->main_hand_attack->base_execute_time = p->main_hand_weapon.swing_time;

    if ( p->off_hand_weapon.type != WEAPON_NONE )
    {
      p->melee_off_hand = debug_cast<melee_t*>( p->find_action( "auto_attack_oh" ) );
      if ( !p->melee_off_hand )
        p->melee_off_hand = new melee_t( "auto_attack_oh", p, sync_weapons );

      p->off_hand_attack = p->melee_off_hand;
      p->off_hand_attack->weapon = &( p->off_hand_weapon );
      p->off_hand_attack->base_execute_time = p->off_hand_weapon.swing_time;
      p->off_hand_attack->id = 1;
    }
  }

  void execute() override
  {
    player->main_hand_attack->schedule_execute();

    if ( player->off_hand_attack )
      player->off_hand_attack->schedule_execute();
  }

  bool ready() override
  {
    if ( player->is_moving() )
      return false;

    return ( player->main_hand_attack->execute_event == nullptr ); // not swinging
  }
};

// Adrenaline Rush ==========================================================

struct adrenaline_rush_t : public rogue_spell_t
{
  timespan_t precombat_seconds;

  adrenaline_rush_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_spell_t( name, p, p->talent.outlaw.adrenaline_rush ),
    precombat_seconds( 0_s )
  {
    add_option( opt_timespan( "precombat_seconds", precombat_seconds ) );
    parse_options( options_str );

    harmful = false;
  }

  void execute() override
  {
    rogue_spell_t::execute();

    // 2020-12-02 - Using over Celerity proc'ed AR does not extend but applies base duration.
    p()->buffs.adrenaline_rush->expire();
    p()->buffs.adrenaline_rush->trigger();
    if ( p()->talent.outlaw.loaded_dice->ok() )
      p()->buffs.loaded_dice->trigger();

    if ( precombat_seconds > 0_s && !p()->in_combat )
    {
      p()->cooldowns.adrenaline_rush->adjust( -precombat_seconds, false );
      p()->buffs.adrenaline_rush->extend_duration( p(), -precombat_seconds );
      p()->buffs.loaded_dice->extend_duration( p(), -precombat_seconds );
    }
  }
};

// Ambush ===================================================================

struct ambush_t : public rogue_attack_t
{
  struct hidden_opportunity_extra_attack_t : public rogue_attack_t
  {
    hidden_opportunity_extra_attack_t( util::string_view name, rogue_t* p ) :
      rogue_attack_t( name, p, p->spec.hidden_opportunity_extra_attack )
    {
    }

    void execute() override
    {
      rogue_attack_t::execute();
      trigger_count_the_odds( execute_state );
    }

    void impact( action_state_t* state ) override
    {
      rogue_attack_t::impact( state );
      trigger_find_weakness( state );
    }

    bool procs_main_gauche() const override
    { return true; } // TOCHECK DFALPHA

    bool procs_blade_flurry() const override
    { return true; } // TOCHECK DFALPHA
  };

  hidden_opportunity_extra_attack_t* extra_attack;

  ambush_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->spell.ambush, options_str ),
    extra_attack( nullptr )
  {
    if ( p->talent.outlaw.hidden_opportunity->ok() )
    {
      extra_attack = p->get_secondary_trigger_action<hidden_opportunity_extra_attack_t>(
        secondary_trigger::HIDDEN_OPPORTUNITY, "ambush_hidden_opportunity" );
      add_child( extra_attack );
    }

    if ( p->talent.assassination.vicious_venoms->ok() )
    {
      add_child( p->active.vicious_venoms.ambush );
    }
  }

  void execute() override
  {
    rogue_attack_t::execute();
    trigger_count_the_odds( execute_state );
    trigger_blindside( execute_state );
    trigger_venom_rush( execute_state );
  }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );

    trigger_find_weakness( state );
    trigger_opportunity( state, extra_attack );
    if ( p()->talent.assassination.vicious_venoms->ok() )
    {
      p()->active.vicious_venoms.ambush->trigger_secondary_action( state->target );
    }
  }

  bool procs_main_gauche() const override
  { return true; }

  bool procs_blade_flurry() const override
  { return true; }
};

// Backstab =================================================================

struct lingering_shadow_t : public rogue_attack_t
{
  lingering_shadow_t( util::string_view name, rogue_t* p ) :
    rogue_attack_t( name, p, p->spec.lingering_shadow_attack )
  {
  }
};

struct backstab_t : public rogue_attack_t
{
  backstab_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->spec.backstab, options_str )
  {
  }

  void init() override
  {
    rogue_attack_t::init();

    if ( !is_secondary_action() )
    {
      if ( p()->active.weaponmaster.backstab )
      {
        add_child( p()->active.weaponmaster.backstab );
      }

      if ( p()->active.lingering_shadow && !p()->talent.subtlety.gloomblade->ok() )
      {
        add_child( p()->active.lingering_shadow );
      }
    }
  }

  double composite_da_multiplier( const action_state_t* state ) const override
  {
    double m = rogue_attack_t::composite_da_multiplier( state );

    if ( p()->position() == POSITION_BACK )
    {
      m *= 1.0 + data().effectN( 4 ).percent();
    }

    return m;
  }

  void execute() override
  {
    rogue_attack_t::execute();

    // 2021-08-30-- Logs appear to show updated behavior of PV and The Rotten benefitting WM procs
    if ( p()->buffs.the_rotten->up() )
    {
      trigger_combo_point_gain( as<int>( p()->buffs.the_rotten->check_value() ), p()->gains.the_rotten );
      p()->buffs.the_rotten->expire( 1_ms );
    }

    p()->buffs.perforated_veins->expire( 1_ms );
  }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );

    trigger_weaponmaster( state, p()->active.weaponmaster.backstab );

    if ( state->result == RESULT_CRIT && p()->talent.subtlety.improved_backstab->ok() && p()->position() == POSITION_BACK )
    {
      timespan_t duration = timespan_t::from_seconds( p()->talent.subtlety.improved_backstab->effectN( 1 ).base_value() );
      trigger_find_weakness( state, duration );
    }

    trigger_lingering_shadow( state );

    if ( p()->talent.subtlety.inevitability->ok() )
    {
      timespan_t extend_duration = timespan_t::from_seconds( p()->talent.subtlety.inevitability->effectN( 2 ).base_value() / 10.0 );
      p()->buffs.symbols_of_death->extend_duration( p(), extend_duration );
    }

    if ( state->result == RESULT_CRIT && p()->set_bonuses.t29_subtlety_4pc->ok() )
    {
      p()->buffs.t29_subtlety_4pc->trigger();
      p()->buffs.t29_subtlety_4pc_black_powder->trigger();
    }
  }

  bool verify_actor_spec() const override
  {
    if ( p()->talent.subtlety.gloomblade->ok() )
      return false;

    return rogue_attack_t::verify_actor_spec();
  }
};

// Between the Eyes =========================================================

struct between_the_eyes_t : public rogue_attack_t
{
  between_the_eyes_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->spec.between_the_eyes, options_str )
  {
    ap_type = attack_power_type::WEAPON_BOTH;
  }

  double composite_crit_chance() const override
  {
    double c = rogue_attack_t::composite_crit_chance();

    if ( p()->buffs.ruthless_precision->check() )
    {
      c += p()->buffs.ruthless_precision->data().effectN( 2 ).percent();
    }

    return c;
  }

  void execute() override
  {
    rogue_attack_t::execute();

    trigger_restless_blades( execute_state );
    trigger_grand_melee( execute_state );

    if ( result_is_hit( execute_state->result ) )
    {
      const auto rs = cast_state( execute_state );
      const int cp_spend = rs->get_combo_points();

      // There is nothing about the debuff duration in spell data, so we have to hardcode the 3s base.
      td( execute_state->target )->debuffs.between_the_eyes->trigger( 3_s * cp_spend );

      if ( p()->spec.greenskins_wickers->ok() )
      {
        // 2022-05-28 -- Greenskins ignores animacharged CP values for calculating proc chance
        if ( rng().roll( rs->get_combo_points( p()->bugs ) * p()->spec.greenskins_wickers->effectN( 1 ).percent() ) )
          p()->buffs.greenskins_wickers->trigger();
      }

      if ( p()->talent.outlaw.ace_up_your_sleeve->ok() )
      {
        if ( rng().roll( p()->talent.outlaw.ace_up_your_sleeve->effectN( 1 ).percent() * cp_spend ) )
        {
          trigger_combo_point_gain( as<int>( p()->talent.outlaw.ace_up_your_sleeve->effectN( 2 ).base_value() ),
                                    p()->gains.ace_up_your_sleeve, true );
        }
      }
    }
  }

  bool procs_blade_flurry() const override
  { return true; }
};

// Blade Flurry =============================================================

struct blade_flurry_attack_t : public rogue_attack_t
{
  blade_flurry_attack_t( util::string_view name, rogue_t* p ) :
    rogue_attack_t( name, p, p->spec.blade_flurry_attack )
  {
    radius = 5;
    range = -1.0;
    aoe = static_cast<int>( p->talent.outlaw.blade_flurry->effectN( 3 ).base_value() +
                            p->talent.outlaw.dancing_steel->effectN( 3 ).base_value() );
  }

  void init() override
  {
    rogue_attack_t::init();
    snapshot_flags |= STATE_TGT_MUL_DA;
  }

  size_t available_targets( std::vector< player_t* >& tl ) const override
  {
    rogue_attack_t::available_targets( tl );

    // Cannot hit the original target.
    tl.erase( std::remove_if( tl.begin(), tl.end(), [ this ]( player_t* t ) { return t == this->target; } ), tl.end() );

    return tl.size();
  }

  bool procs_poison() const override
  { return true; }
};

struct blade_flurry_t : public rogue_attack_t
{
  struct blade_flurry_instant_attack_t : public rogue_attack_t
  {
    blade_flurry_instant_attack_t( util::string_view name, rogue_t* p ) :
      rogue_attack_t( name, p, p->spec.blade_flurry_instant_attack )
    {
      range = -1.0;
    }
  };

  blade_flurry_instant_attack_t* instant_attack;

  blade_flurry_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->talent.outlaw.blade_flurry, options_str ),
    instant_attack( nullptr )
  {
    harmful = false;

    if ( p->spec.blade_flurry_attack->ok() )
    {
      add_child( p->active.blade_flurry );
    }

    if ( p->spec.blade_flurry_instant_attack->ok() )
    {
      instant_attack = p->get_background_action<blade_flurry_instant_attack_t>( "blade_flurry_instant_attack" );
      add_child( instant_attack );
    }
  }

  void execute() override
  {
    rogue_attack_t::execute();
    p()->buffs.blade_flurry->trigger();

    if ( instant_attack )
    {
      instant_attack->set_target( target );
      instant_attack->execute();
    }
  }
};

// Blade Rush ===============================================================

struct blade_rush_t : public rogue_attack_t
{
  struct blade_rush_attack_t : public rogue_attack_t
  {
    blade_rush_attack_t( util::string_view name, rogue_t* p ) :
      rogue_attack_t( name, p, p->spec.blade_rush_attack )
    {
      dual = true;
      aoe = -1;
    }

    void execute() override
    {
      rogue_attack_t::execute();
      p()->buffs.blade_rush->trigger();
    }

    double composite_da_multiplier( const action_state_t* state ) const override
    {
      double m = rogue_attack_t::composite_da_multiplier( state );

      if ( state->target == state->action->target )
        m *= data().effectN( 2 ).percent();
      else if ( p()->buffs.blade_flurry->up() )
        m *= 1.0 + p()->talent.outlaw.blade_rush->effectN( 1 ).percent();

      return m;
    }
  };

  blade_rush_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->talent.outlaw.blade_rush, options_str )
  {
    execute_action = p->get_background_action<blade_rush_attack_t>( "blade_rush_attack" );
    execute_action->stats = stats;
  }

  bool procs_main_gauche() const override
  { return true; }
};

// Bloodfang Legendary ======================================================

struct bloodfang_t : public rogue_attack_t
{
  bloodfang_t( util::string_view name, rogue_t* p ) :
    rogue_attack_t( name, p, p->legendary.essence_of_bloodfang->effectN( 1 ).trigger() )
  {
    internal_cooldown->duration = p->legendary.essence_of_bloodfang->internal_cooldown();
  }
};

// Cold Blood ===============================================================

struct cold_blood_t : public rogue_spell_t
{
  double precombat_seconds;

  cold_blood_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_spell_t( name, p, p->talent.rogue.cold_blood ),
    precombat_seconds( 0.0 )
  {
    add_option( opt_float( "precombat_seconds", precombat_seconds ) );
    parse_options( options_str );

    harmful = false;
  }

  void execute() override
  {
    rogue_spell_t::execute();

    if ( precombat_seconds && !p()->in_combat )
    {
      p()->cooldowns.cold_blood->adjust( -timespan_t::from_seconds( precombat_seconds ), false );
    }

    p()->buffs.cold_blood->trigger();
  }
};

// Crimson Tempest ==========================================================

struct crimson_tempest_t : public rogue_attack_t
{
  crimson_tempest_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->talent.assassination.crimson_tempest, options_str )
  {
    aoe = -1;
    reduced_aoe_targets = data().effectN( 3 ).base_value();
  }

  timespan_t composite_dot_duration( const action_state_t* s ) const override
  {
    const auto rs = cast_state( s );
    timespan_t duration = data().duration() * ( 1 + rs->get_combo_points() );
    duration *= 1.0 / rs->get_exsanguinated_rate();

    return duration;
  }

  double combo_point_da_multiplier( const action_state_t* s ) const override
  {
    return static_cast<double>( cast_state( s )->get_combo_points() ) + 1.0;
  }

  bool procs_poison() const override
  { return true; }
};

// Deathmark ================================================================

struct deathmark_t : public rogue_attack_t
{
  deathmark_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->talent.assassination.deathmark, options_str )
  {
  }

  void init() override
  {
    rogue_attack_t::init();

    if ( data().ok() )
    {
      add_child( p()->active.deathmark.garrote );
      add_child( p()->active.deathmark.rupture );
      add_child( p()->active.deathmark.amplifying_poison );
      add_child( p()->active.deathmark.deadly_poison_dot );
      add_child( p()->active.deathmark.deadly_poison_instant );
      add_child( p()->active.deathmark.instant_poison );
      add_child( p()->active.deathmark.wound_poison );
    }
  }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );

    auto tdata = td( state->target );
    tdata->debuffs.deathmark->trigger();
    
    // Deathmark clones all the relevant DoTs and debuffs present on the target when cast
    if ( tdata->dots.deadly_poison->is_ticking() )
      tdata->dots.deadly_poison->copy( state->target, DOT_COPY_CLONE, p()->active.deathmark.deadly_poison_dot );
    
    if ( tdata->dots.garrote->is_ticking() )
      tdata->dots.garrote->copy( state->target, DOT_COPY_CLONE, p()->active.deathmark.garrote );
    
    if ( tdata->dots.rupture->is_ticking() )
      tdata->dots.rupture->copy( state->target, DOT_COPY_CLONE, p()->active.deathmark.rupture );

    if ( tdata->debuffs.amplifying_poison->check() )
    {
      tdata->debuffs.amplifying_poison_deathmark->expire();
      tdata->debuffs.amplifying_poison_deathmark->trigger( tdata->debuffs.amplifying_poison->check(),
                                                           tdata->debuffs.amplifying_poison->remains() );
    }
  }

  void last_tick( dot_t* d ) override
  {
    rogue_attack_t::last_tick( d );

    // Debuffs triggered by Deathmark currently expire immediately when Deathmark fades
    auto tdata = td( d->target );
    tdata->dots.deadly_poison_deathmark->cancel();
    tdata->dots.garrote_deathmark->cancel();
    tdata->dots.rupture_deathmark->cancel();
    tdata->debuffs.amplifying_poison_deathmark->expire();
  }
};

// Detection ================================================================

// This ability does nothing but for some odd reasons throughout the history of Rogue spaghetti, we may want to look at using it. So, let's support it.
struct detection_t : public rogue_spell_t
{
  detection_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_spell_t( name, p, p->spell.detection, options_str )
  {
    gcd_type = gcd_haste_type::ATTACK_HASTE;
    min_gcd = 750_ms; // Force 750ms min gcd because rogue player base has a 1s min.
  }
};

// Dispatch =================================================================

struct dispatch_t: public rogue_attack_t
{
  dispatch_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->spec.dispatch, options_str )
  {
  }

  double cost() const override
  {
    double c = rogue_attack_t::cost();

    if ( p()->buffs.summarily_dispatched->check() )
    {
      c += p()->buffs.summarily_dispatched->check() * p()->spec.summarily_dispatched_buff->effectN( 2 ).base_value();
    }

    return c;
  }

  void execute() override
  {
    rogue_attack_t::execute();

    // TOCHECK DFALPHA -- Verify Echoing Reprimand works correctly
    if ( p()->talent.outlaw.summarily_dispatched->ok() )
    {
      int cp = cast_state( execute_state )->get_combo_points();
      if ( cp >= p()->talent.outlaw.summarily_dispatched->effectN( 2 ).base_value() )
      {
        p()->buffs.summarily_dispatched->trigger();
      }
    }

    if ( p()->set_bonuses.t29_outlaw_2pc->ok() )
    {
      p()->buffs.t29_outlaw_2pc->expire();
      p()->buffs.t29_outlaw_2pc->trigger( cast_state( execute_state )->get_combo_points() );
    }

    trigger_restless_blades( execute_state );
    trigger_grand_melee( execute_state );
    trigger_count_the_odds( execute_state );
  }

  bool procs_main_gauche() const override
  { return true; }

  bool procs_blade_flurry() const override
  { return true; }
};

// Dreadblades ==============================================================

struct dreadblades_t : public rogue_attack_t
{
  dreadblades_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->talent.outlaw.dreadblades, options_str )
  {}

  void execute() override
  {
    rogue_attack_t::execute();
    p()->buffs.dreadblades->trigger();
  }

  bool procs_main_gauche() const override
  { return true; }

  bool procs_blade_flurry() const override
  { return true; }
};

// Envenom ==================================================================

struct envenom_t : public rogue_attack_t
{
  envenom_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->spec.envenom, options_str )
  {
    dot_duration = timespan_t::zero();
    affected_by.lethal_dose = false;
    affected_by.zoldyck_insignia = false;
  }

  double composite_target_multiplier( player_t* target ) const override
  {
    double m = rogue_attack_t::composite_target_multiplier( target );

    if ( p()->legendary.doomblade.ok() )
    {
      // 2021-03-04-- 9.0.5 notes now specify SBS works with Doomblade
      rogue_td_t* tdata = td( target );
      int active_dots = tdata->dots.garrote->is_ticking() + tdata->dots.rupture->is_ticking() +
        tdata->dots.crimson_tempest->is_ticking() + tdata->dots.internal_bleeding->is_ticking() +
        tdata->dots.mutilated_flesh->is_ticking() + tdata->dots.serrated_bone_spike->is_ticking();

      m *= 1.0 + p()->legendary.doomblade->effectN( 2 ).percent() * active_dots;
    }

    if ( p()->talent.assassination.amplifying_poison->ok() )
    {
      const int consume_stacks = as<int>( p()->talent.assassination.amplifying_poison->effectN( 2 ).base_value() );
      rogue_td_t* tdata = td( target );

      if ( tdata->debuffs.amplifying_poison->stack() >= consume_stacks )
      {
        m *= 1.0 + p()->talent.assassination.amplifying_poison->effectN( 1 ).percent();
      }

      if ( tdata->debuffs.amplifying_poison_deathmark->stack() >= consume_stacks )
      {
        m *= 1.0 + p()->talent.assassination.amplifying_poison->effectN( 1 ).percent();
      }
    }

    return m;
  }

  void execute() override
  {
    rogue_attack_t::execute();
    trigger_poison_bomb( execute_state );

    // TOCHECK -- If this consumes on execute or impact when parried
    if ( p()->talent.assassination.amplifying_poison->ok() )
    {
      const int consume_stacks = as<int>( p()->talent.assassination.amplifying_poison->effectN( 2 ).base_value() );
      rogue_td_t* tdata = td( target );

      if ( tdata->debuffs.amplifying_poison->stack() >= consume_stacks )
      {
        tdata->debuffs.amplifying_poison->decrement( consume_stacks );
        p()->procs.amplifying_poison_consumed->occur();
      }

      if ( tdata->debuffs.amplifying_poison_deathmark->stack() >= consume_stacks )
      {
        tdata->debuffs.amplifying_poison_deathmark->decrement( consume_stacks );
        p()->procs.amplifying_poison_deathmark_consumed->occur();
      }
    }
  }

  void impact( action_state_t* state ) override
  {
    // Trigger Envenom buff before impact() so that poison procs from Envenom itself benefit
    timespan_t envenom_duration = p()->buffs.envenom->data().duration() * ( 1 + cast_state( state )->get_combo_points() ) +
                                  p()->talent.assassination.twist_the_knife->effectN( 1 ).time_value();
    p()->buffs.envenom->trigger( envenom_duration );

    rogue_attack_t::impact( state );

    if ( p()->talent.assassination.cut_to_the_chase->ok() && p()->buffs.slice_and_dice->check() )
    {
      double extend_duration = p()->talent.assassination.cut_to_the_chase->effectN( 1 ).base_value() * cast_state( state )->get_combo_points();
      // Max duration it extends to is the maximum possible full pandemic duration, i.e. around 46s without and 54s with Deeper Stratagem.
      timespan_t max_duration = ( p()->consume_cp_max() + 1 ) * p()->buffs.slice_and_dice->data().duration() * 1.3;
      timespan_t effective_extend = std::min( timespan_t::from_seconds( extend_duration ), max_duration - p()->buffs.slice_and_dice->remains() );
      p()->buffs.slice_and_dice->extend_duration( p(), effective_extend );
    }
  }
};

// Eviscerate ===============================================================

struct eviscerate_t : public rogue_attack_t
{
  struct eviscerate_bonus_t : public rogue_attack_t
  {
    int last_eviscerate_cp;

    eviscerate_bonus_t( util::string_view name, rogue_t* p ):
      rogue_attack_t( name, p, p->spec.eviscerate_shadow_attack ),
      last_eviscerate_cp( 1 )
    {
      if ( p->talent.subtlety.shadowed_finishers->ok() )
      {
        // Spell has the full damage coefficient and is modified via talent scripting
        base_multiplier *= p->talent.subtlety.shadowed_finishers->effectN( 1 ).percent();
      }
    }

    void reset() override
    {
      rogue_attack_t::reset();
      last_eviscerate_cp = 1;
    }

    double combo_point_da_multiplier( const action_state_t* ) const override
    {
      return as<double>( last_eviscerate_cp );
    }
  };

  eviscerate_bonus_t* bonus_attack;

  eviscerate_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ):
    rogue_attack_t( name, p, p->spec.eviscerate, options_str ),
    bonus_attack( nullptr )
  {
    if ( p->talent.subtlety.shadowed_finishers->ok() )
    {
      bonus_attack = p->get_background_action<eviscerate_bonus_t>( "eviscerate_bonus" );
      add_child( bonus_attack );
    }
  }

  void execute() override
  {
    rogue_attack_t::execute();
    p()->buffs.deeper_daggers->trigger(); // TOCHECK: Does this happen before or after the bonus damage? Currently before. (-SL Launch)

    if ( bonus_attack && td( target )->debuffs.find_weakness->up() )
    {
      bonus_attack->last_eviscerate_cp = cast_state( execute_state )->get_combo_points();
      bonus_attack->set_target( target );
      bonus_attack->execute();
    }

    if ( p()->spec.finality_eviscerate_buff->ok() )
    {
      if ( p()->buffs.finality_eviscerate->check() )
        p()->buffs.finality_eviscerate->expire();
      else
        p()->buffs.finality_eviscerate->trigger();
    }

    if ( p()->set_bonuses.t29_subtlety_2pc->ok() )
    {
      p()->buffs.t29_subtlety_2pc->expire();
      p()->buffs.t29_subtlety_2pc->trigger( cast_state( execute_state )->get_combo_points() );
    }
  }
};

// Exsanguinate =============================================================

struct exsanguinate_t : public rogue_attack_t
{
  exsanguinate_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ):
    rogue_attack_t( name, p, p->talent.assassination.exsanguinate, options_str )
  { }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );
    p()->trigger_exsanguinate( state->target );
  }
};

// Fan of Knives ============================================================

struct fan_of_knives_t: public rogue_attack_t
{
  fan_of_knives_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ):
    rogue_attack_t( name, p, p->spec.fan_of_knives, options_str )
  {
    energize_type     = action_energize::ON_HIT;
    energize_resource = RESOURCE_COMBO_POINT;
    energize_amount   = data().effectN( 2 ).base_value();
    // 2021-10-07 - Not in the whitelist but confirmed as working as of 9.1.5 PTR
    affected_by.shadow_blades_cp = true;

    aoe = -1;
    reduced_aoe_targets = data().effectN( 3 ).base_value();
  }

  double action_multiplier() const override
  {
    double m = rogue_attack_t::action_multiplier();

    if ( p()->talent.assassination.flying_daggers.ok() )
    {
      if ( target_list().size() >= p()->talent.assassination.flying_daggers->effectN( 2 ).base_value() )
      {
        m *= 1.0 + p()->talent.assassination.flying_daggers->effectN( 1 ).percent();
      }
    }

    return m;
  }

  bool procs_poison() const override
  { return true; }

  // 2021-10-07 - Works as of 9.1.5 PTR
  bool procs_shadow_blades_damage() const override
  { return true; }
};

// Feint ====================================================================

struct feint_t : public rogue_attack_t
{
  feint_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ):
    rogue_attack_t( name, p, p->talent.rogue.feint, options_str )
  { }

  void execute() override
  {
    rogue_attack_t::execute();
    p()->buffs.feint->trigger();
  }
};

// Garrote ==================================================================

struct garrote_t : public rogue_attack_t
{
  garrote_t( util::string_view name, rogue_t* p, const spell_data_t* s, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, s, options_str )
  {
  }

  int n_targets() const override
  {
    int n = rogue_attack_t::n_targets();

    if ( !is_secondary_action() && p()->buffs.indiscriminate_carnage_garrote->check() )
    {
      n = as<int>( p()->talent.assassination.indiscriminate_carnage->effectN( 1 ).base_value() );
    }

    return n;
  }

  double composite_persistent_multiplier( const action_state_t* state ) const override
  {
    double m = rogue_attack_t::composite_persistent_multiplier( state );

    if ( p()->talent.assassination.improved_garrote->ok() )
    {
      m *= 1.0 + p()->buffs.improved_garrote->stack_value();
      m *= 1.0 + p()->buffs.improved_garrote_aura->stack_value();
    }

    return m;
  }

  timespan_t composite_dot_duration( const action_state_t* s ) const override
  {
    timespan_t duration = rogue_attack_t::composite_dot_duration( s );
    duration *= 1.0 / cast_state( s )->get_exsanguinated_rate();
    return duration;
  }

  void tick( dot_t* d ) override
  {
    rogue_attack_t::tick( d );
    trigger_venomous_wounds( d->state );
  }

  void execute() override
  {
    rogue_attack_t::execute();

    // DFALPHA TOCHECK -- Does not work with Shadow Dance currently based on testing but supposed to
    if ( p()->talent.assassination.shrouded_suffocation->ok() &&
         p()->stealthed( STEALTH_BASIC | STEALTH_ROGUE | STEALTH_IMPROVED_GARROTE ) )
    {
      trigger_combo_point_gain( as<int>( p()->talent.assassination.shrouded_suffocation->effectN( 2 ).base_value() ),
                                p()->gains.shrouded_suffocation );
    }

    p()->buffs.indiscriminate_carnage_garrote->expire();
  }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );

    if ( !is_secondary_action() && td( state->target )->dots.deathmark->is_ticking() )
    {
      p()->active.deathmark.garrote->trigger_secondary_action( state->target );
    }
  }

  void update_ready( timespan_t cd_duration = timespan_t::min() ) override
  {
    if ( p()->talent.assassination.improved_garrote->ok() )
    {
      if ( p()->buffs.improved_garrote->check() || p()->buffs.improved_garrote_aura->check() )
      {
        cd_duration = timespan_t::zero();
      }
    }

    rogue_attack_t::update_ready( cd_duration );
  }
};

// Gouge =====================================================================

struct gouge_t : public rogue_attack_t
{
  gouge_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->talent.rogue.gouge, options_str )
  {
  }

  bool target_ready( player_t* candidate_target ) override
  {
    if ( !candidate_target->is_boss() )
      return false;

    return rogue_attack_t::target_ready( candidate_target );
  }
};

// Ghostly Strike ===========================================================

struct ghostly_strike_t : public rogue_attack_t
{
  ghostly_strike_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->talent.outlaw.ghostly_strike, options_str )
  {
  }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );

    if ( result_is_hit( state->result ) )
    {
      td( state->target )->debuffs.ghostly_strike->trigger();
    }
  }

  bool procs_main_gauche() const override
  { return true; }

  bool procs_blade_flurry() const override
  { return true; }
};


// Gloomblade ===============================================================

struct gloomblade_t : public rogue_attack_t
{
  gloomblade_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->talent.subtlety.gloomblade, options_str )
  {
  }

  void init() override
  {
    rogue_attack_t::init();

    if ( !is_secondary_action() )
    {
      if ( p()->active.weaponmaster.gloomblade )
      {
        add_child( p()->active.weaponmaster.gloomblade );
      }

      if ( p()->active.lingering_shadow && p()->talent.subtlety.gloomblade->ok() )
      {
        add_child( p()->active.lingering_shadow );
      }
    }
  }

  void execute() override
  {
    rogue_attack_t::execute();

    // 2021-08-30-- Logs appear to show updated behavior of PV and The Rotten benefitting WM procs
    if ( p()->buffs.the_rotten->up() )
    {
      trigger_combo_point_gain( as<int>( p()->buffs.the_rotten->check_value() ), p()->gains.the_rotten );
      p()->buffs.the_rotten->expire( 1_ms );
    }

    p()->buffs.perforated_veins->expire( 1_ms );
  }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );

    trigger_weaponmaster( state, p()->active.weaponmaster.gloomblade );

    if ( state->result == RESULT_CRIT && p()->talent.subtlety.improved_backstab->ok() )
    {
      timespan_t duration = timespan_t::from_seconds( p()->talent.subtlety.improved_backstab->effectN( 1 ).base_value() );
      trigger_find_weakness( state, duration );
    }

    trigger_lingering_shadow( state );

    if ( p()->talent.subtlety.inevitability->ok() )
    {
      timespan_t extend_duration = timespan_t::from_seconds( p()->talent.subtlety.inevitability->effectN( 2 ).base_value() / 10.0 );
      p()->buffs.symbols_of_death->extend_duration( p(), extend_duration );
    }

    if ( state->result == RESULT_CRIT && p()->set_bonuses.t29_subtlety_4pc->ok() )
    {
      p()->buffs.t29_subtlety_4pc->trigger();
      p()->buffs.t29_subtlety_4pc_black_powder->trigger();
    }
  }
};

// Indiscriminate Carnage ===================================================

struct indiscriminate_carnage_t : public rogue_spell_t
{
  indiscriminate_carnage_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_spell_t( name, p, p->talent.assassination.indiscriminate_carnage, options_str )
  {
    harmful = false;
  }

  void execute() override
  {
    rogue_spell_t::execute();
    p()->buffs.indiscriminate_carnage_garrote->trigger();
    p()->buffs.indiscriminate_carnage_rupture->trigger();
  }

  bool ready() override
  {
    // Cooldown does not begin until both buffs are consumed
    if ( p()->buffs.indiscriminate_carnage_garrote->check() ||
         p()->buffs.indiscriminate_carnage_rupture->check() )
      return false;

    return rogue_spell_t::ready();
  }
};

// Kick =====================================================================

struct kick_t : public rogue_attack_t
{
  kick_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) : 
    rogue_attack_t( name, p, p->spell.kick, options_str )
  {
    is_interrupt = true;
  }

  bool target_ready( player_t* candidate_target ) override
  {
    if ( candidate_target->debuffs.casting && !candidate_target->debuffs.casting->check() )
      return false;

    return rogue_attack_t::target_ready( candidate_target );
  }
};

// Killing Spree ============================================================

struct killing_spree_tick_t : public rogue_attack_t
{
  killing_spree_tick_t( util::string_view name, rogue_t* p, const spell_data_t* s ) :
    rogue_attack_t( name, p, s )
  {
    direct_tick = true;
  }

  bool procs_main_gauche() const override
  { return weapon->slot == SLOT_MAIN_HAND; }

  // OH hits do not proc Combat Potency
  bool procs_fatal_flourish() const override
  { return false; }

  bool procs_blade_flurry() const override
  { return true; }
};

struct killing_spree_t : public rogue_attack_t
{
  melee_attack_t* attack_mh;
  melee_attack_t* attack_oh;

  killing_spree_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->talent.outlaw.killing_spree, options_str ),
    attack_mh( nullptr ), attack_oh( nullptr )
  {
    channeled = tick_zero = true;
    interrupt_auto_attack = false;

    attack_mh = p->get_background_action<killing_spree_tick_t>( "killing_spree_mh", p->spec.killing_spree_mh_attack );
    attack_oh = p->get_background_action<killing_spree_tick_t>( "killing_spree_oh", p->spec.killing_spree_oh_attack );
    add_child( attack_mh );
    add_child( attack_oh );
  }

  timespan_t tick_time( const action_state_t* ) const override
  { return base_tick_time; }

  void execute() override
  {
    p()->buffs.killing_spree->trigger();

    rogue_attack_t::execute();
  }

  void tick( dot_t* d ) override
  {
    rogue_attack_t::tick( d );

    // If Blade Flurry is up, pick a random target per tick, otherwise use the primary target
    // On static dummies, this appears to have cycling for selection, but in real-world situations it is likely effectively random
    // TODO: Analyze dungeon logs to see if there is any deterministic pattern here with moving enemies
    player_t* tick_target = d->target;
    if ( p()->buffs.blade_flurry->check() )
    {
      auto candidate_targets = targets_in_range_list( target_list() );
      tick_target = candidate_targets[ rng().range( candidate_targets.size() ) ];
    }

    attack_mh->set_target( tick_target );
    attack_oh->set_target( tick_target );
    attack_mh->execute();
    attack_oh->execute();
  }
};

// Kingsbane ================================================================

struct kingsbane_t : public rogue_attack_t
{
  kingsbane_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->talent.assassination.kingsbane, options_str )
  {
  }

  void last_tick( dot_t* d ) override
  {
    rogue_attack_t::last_tick( d );
    p()->buffs.kingsbane->expire();
  }
};

// Pistol Shot ==============================================================

struct pistol_shot_t : public rogue_attack_t
{
  pistol_shot_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->spec.pistol_shot, options_str )
  {
    ap_type = attack_power_type::WEAPON_BOTH;
  }

  void init() override
  {
    rogue_attack_t::init();

    if ( !is_secondary_action() )
    {
      if ( p()->active.concealed_blunderbuss )
      {
        add_child( p()->active.concealed_blunderbuss );
      }
      if ( p()->active.fan_the_hammer )
      {
        add_child( p()->active.fan_the_hammer );
      }
    }
  }

  double cost() const override
  {
    double c = rogue_attack_t::cost();

    if ( p()->buffs.opportunity->check() )
    {
      c *= 1.0 + p()->buffs.opportunity->data().effectN( 1 ).percent();
    }

    return c;
  }

  double action_multiplier() const override
  {
    double m = rogue_attack_t::action_multiplier();

    m *= 1.0 + p()->buffs.opportunity->value();
    m *= 1.0 + p()->buffs.greenskins_wickers->value();

    return m;
  }

  double generate_cp() const override
  {
    double g = rogue_attack_t::generate_cp();
    if ( g == 0.0 )
      return 0.0;

    if ( p()->talent.outlaw.quick_draw->ok() && p()->buffs.opportunity->check() )
    {
      g += p()->talent.outlaw.quick_draw->effectN( 2 ).base_value();
    }

    return g;
  }

  void execute() override
  {
    rogue_attack_t::execute();

    // Opportunity-Triggered Mechanics
    if ( p()->buffs.opportunity->check() )
    {
      if ( generate_cp() > 0 && p()->talent.outlaw.quick_draw->ok() )
      {
        const int cp_gain = as<int>( p()->talent.outlaw.quick_draw->effectN( 2 ).base_value() );
        trigger_combo_point_gain( cp_gain, p()->gains.quick_draw );
      }

      // Audacity -- Currently on beta this can also trigger from Fan the Hammer procs
      if ( p()->talent.outlaw.audacity->ok() )
      {
        p()->buffs.audacity->trigger( 1, buff_t::DEFAULT_VALUE(), p()->extra_attack_proc_chance() );
      }

      if ( p()->set_bonuses.t29_outlaw_4pc->ok() )
      {
        p()->buffs.t29_outlaw_4pc->trigger();
      }
    }

    p()->buffs.opportunity->decrement();
    p()->buffs.greenskins_wickers->expire();

    // Concealed Blunderbuss Legendary
    if ( p()->active.concealed_blunderbuss && !is_secondary_action() )
    {
      unsigned int num_shots = as<unsigned>( p()->buffs.concealed_blunderbuss->value() );
      for ( unsigned i = 0; i < num_shots; ++i )
      {
        p()->active.concealed_blunderbuss->trigger_secondary_action( execute_state->target, 0.1_s * ( 1 + i ) );
      }
      p()->buffs.concealed_blunderbuss->expire();
    }

    // Fan the Hammer
    if ( p()->active.fan_the_hammer && !is_secondary_action() )
    {
      unsigned int num_shots = std::min( p()->buffs.opportunity->check(),
                                         as<int>( p()->talent.outlaw.fan_the_hammer->effectN( 1 ).base_value() ) );
      for ( unsigned i = 0; i < num_shots; ++i )
      {
        p()->active.fan_the_hammer->trigger_secondary_action( execute_state->target, 0.1_s * ( 1 + i ) );
      }
    }
  }

  // As of DF Beta neither Blunderbuss or Fan the Hammer trigger this
  bool procs_fatal_flourish() const override
  { return !is_secondary_action(); }

  // As of DF Beta neither Blunderbuss or Fan the Hammer trigger this
  bool procs_blade_flurry() const override
  { return !is_secondary_action(); }
};

// Main Gauche ==============================================================

struct main_gauche_t : public rogue_attack_t
{
  main_gauche_t( util::string_view name, rogue_t* p ) :
    rogue_attack_t( name, p, p->mastery.main_gauche_attack )
  {
  }

  double attack_direct_power_coefficient( const action_state_t* s ) const override
  {
    double ap = rogue_attack_t::attack_direct_power_coefficient( s );

    // Main Gauche is not set up like most masteries as it is a flat mod instead of a percent mod
    // Need to reference the 2nd effect and directly apply the sp_coeff instead of dividing by 100
    ap += p()->cache.mastery() * p()->mastery.main_gauche->effectN( 2 ).sp_coeff();

    return ap;
  }

  bool procs_fatal_flourish() const override
  { return true; }

  bool procs_poison() const override
  { return true; }

  bool procs_blade_flurry() const override
  { return true; }
};

// Marked for Death =========================================================

struct marked_for_death_t : public rogue_spell_t
{
  double precombat_seconds;

  marked_for_death_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ):
    rogue_spell_t( name, p, p->talent.rogue.marked_for_death ),
    precombat_seconds( 0.0 )
  {
    add_option( opt_float( "precombat_seconds", precombat_seconds ) );
    parse_options( options_str );

    harmful = false;
    energize_type = action_energize::ON_CAST;
  }

  void execute() override
  {
    rogue_spell_t::execute();

    if ( precombat_seconds && ! p() -> in_combat ) {
      p()->cooldowns.marked_for_death->adjust( -timespan_t::from_seconds( precombat_seconds ), false );
    }
  }

  void impact( action_state_t* state ) override
  {
    rogue_spell_t::impact( state );

    td( state->target )->debuffs.marked_for_death->trigger();
  }
};

// Mutilate =================================================================

struct mutilate_t : public rogue_attack_t
{
  struct mutilate_strike_t : public rogue_attack_t
  {
    // Note: Uses spell_t instead of rogue_spell_t to avoid action_state casting issues
    struct doomblade_t : public residual_action::residual_periodic_action_t<spell_t>
    {
      rogue_t* rogue;

      doomblade_t( util::string_view name, rogue_t* p ) :
        base_t( name, p, p->spec.doomblade_debuff ), rogue( p )
      {
        dual = true;
      }
    };

    doomblade_t* doomblade_dot;

    mutilate_strike_t( util::string_view name, rogue_t* p, const spell_data_t* s ) :
      rogue_attack_t( name, p, s ),
      doomblade_dot( nullptr )
    {
      if ( p->spec.doomblade_debuff->ok() )
      {
        doomblade_dot = p->get_background_action<doomblade_t>( "mutilated_flesh" );
      }
    }

    double action_multiplier() const override
    {
      double m = rogue_attack_t::action_multiplier();

      // Appears to be overridden by a scripted multiplier even though the base damage is identical
      if ( secondary_trigger_type == secondary_trigger::VICIOUS_VENOMS )
      {
        m *= p()->talent.assassination.vicious_venoms->effectN( 1 ).percent();
      }

      return m;
    }

    void impact( action_state_t* state ) override
    {
      rogue_attack_t::impact( state );
      trigger_seal_fate( state );

      if ( doomblade_dot && result_is_hit( state->result ) )
      {
        const double dot_damage = state->result_amount * ( p()->talent.assassination.doomblade->ok() ?
                                                           p()->talent.assassination.doomblade->effectN( 1 ).percent() :
                                                           p()->legendary.doomblade->effectN( 1 ).percent() );
        residual_action::trigger( doomblade_dot, state->target, dot_damage );
      }
    }

    // 2021-10-07 - Works as of 9.1.5 PTR
    bool procs_shadow_blades_damage() const override
    { return true; }
  };

  mutilate_strike_t* mh_strike;
  mutilate_strike_t* oh_strike;

  mutilate_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->spec.mutilate, options_str ),
    mh_strike( nullptr ), oh_strike( nullptr )
  {
    if ( p->main_hand_weapon.type != WEAPON_DAGGER || p->off_hand_weapon.type != WEAPON_DAGGER )
    {
      sim->errorf( "Player %s attempting to execute Mutilate without two daggers equipped.", p->name() );
      background = true;
    }

    mh_strike = p->get_background_action<mutilate_strike_t>( "mutilate_mh", data().effectN( 3 ).trigger() );
    oh_strike = p->get_background_action<mutilate_strike_t>( "mutilate_oh", data().effectN( 4 ).trigger() );
    add_child( mh_strike );
    add_child( oh_strike );

    if ( p->talent.assassination.vicious_venoms->ok() )
    {
      add_child( p->active.vicious_venoms.mutilate_mh );
      add_child( p->active.vicious_venoms.mutilate_oh );
    }

    if ( mh_strike->doomblade_dot )
    {
      add_child( mh_strike->doomblade_dot );
    }
  }

  void execute() override
  {
    rogue_attack_t::execute();

    if ( result_is_hit( execute_state->result ) )
    {
      mh_strike->set_target( execute_state->target );
      mh_strike->execute();

      oh_strike->set_target( execute_state->target );
      oh_strike->execute();

      if ( p()->talent.assassination.vicious_venoms->ok() )
      {
        p()->active.vicious_venoms.mutilate_mh->trigger_secondary_action( execute_state->target );
        p()->active.vicious_venoms.mutilate_oh->trigger_secondary_action( execute_state->target );
      }

      trigger_blindside( execute_state );
      trigger_venom_rush( execute_state );
    }
  }

  bool has_amount_result() const override
  { return true; }
};

// Roll the Bones ===========================================================

struct roll_the_bones_t : public rogue_spell_t
{
  timespan_t precombat_seconds;

  roll_the_bones_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_spell_t( name, p, p->talent.outlaw.roll_the_bones ),
    precombat_seconds( 0_s )
  {
    add_option( opt_timespan( "precombat_seconds", precombat_seconds ) );
    parse_options( options_str );

    harmful = false;
    dot_duration = timespan_t::zero();
  }

  void execute() override
  {
    rogue_spell_t::execute();

    timespan_t d = p()->buffs.roll_the_bones->data().duration();
    if ( precombat_seconds > 0_s && !p()->in_combat )
      d -= precombat_seconds;

    p()->buffs.roll_the_bones->trigger( d );
  }
};

// Rupture ==================================================================

struct rupture_t : public rogue_attack_t
{
  struct replicating_shadows_tick_t : public rogue_attack_t
  {
    replicating_shadows_tick_t( util::string_view name, rogue_t* p ) :
      rogue_attack_t( name, p, p->spec.replicating_shadows_tick )
    {
    }

    bool procs_poison() const override
    { return false; }
  };

  replicating_shadows_tick_t* replicating_shadows_tick;
  
  rupture_t( util::string_view name, rogue_t* p, const spell_data_t* s, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, s, options_str ),
    replicating_shadows_tick( nullptr )
  {
    if ( p->talent.subtlety.replicating_shadows->ok() )
    {
      replicating_shadows_tick = p->get_background_action<replicating_shadows_tick_t>( "rupture_replicating_shadows" );
      add_child( replicating_shadows_tick );
    }
  }

  int n_targets() const override
  {
    int n = rogue_attack_t::n_targets();

    if ( !is_secondary_action() && p()->buffs.indiscriminate_carnage_rupture->check() )
    {
      n = as<int>( p()->talent.assassination.indiscriminate_carnage->effectN( 1 ).base_value() );
    }

    return n;
  }

  timespan_t composite_dot_duration( const action_state_t* s ) const override
  {
    const auto rs = cast_state( s );
    timespan_t duration = data().duration() * ( 1 + rs->get_combo_points() );
    duration *= 1.0 / rs->get_exsanguinated_rate();
    return duration;
  }

  double composite_persistent_multiplier( const action_state_t* state ) const override
  {
    double m = rogue_attack_t::composite_persistent_multiplier( state );
    m *= 1.0 + p()->buffs.finality_rupture->value();
    return m;
  }

  void execute() override
  {
    rogue_attack_t::execute();

    trigger_scent_of_blood();

    if ( p()->spec.finality_rupture_buff->ok() )
    {
      if ( p()->buffs.finality_rupture->check() )
        p()->buffs.finality_rupture->expire();
      else
        p()->buffs.finality_rupture->trigger();
    }

    p()->buffs.indiscriminate_carnage_rupture->expire();
  }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );

    if ( !is_secondary_action() && td( state->target )->dots.deathmark->is_ticking() )
    {
      p()->active.deathmark.rupture->trigger_secondary_action( state->target,
                                                               cast_state( state )->get_combo_points() );
    }
  }

  void tick( dot_t* d ) override
  {
    rogue_attack_t::tick( d );
    trigger_venomous_wounds( d->state );

    if ( replicating_shadows_tick )
    {
      replicating_shadows_tick->execute_on_target( d->target );
    }
  }

  void last_tick( dot_t* d ) override
  {
    rogue_attack_t::last_tick( d );

    // Delay to allow the demise to reset() and update get_active_dots() count
    make_event( *p()->sim, [this] { trigger_scent_of_blood(); } );
  }

  std::unique_ptr<expr_t> create_expression( util::string_view name ) override
  {
    if ( util::str_compare_ci( name, "new_duration" ) )
    {
      struct new_duration_expr_t : public expr_t
      {
        rupture_t* rupture;
        double cp_max_spend;
        double base_duration;

        new_duration_expr_t( rupture_t* a ) :
          expr_t( "new_duration" ),
          rupture( a ),
          cp_max_spend( a->p()->consume_cp_max() ),
          base_duration( a->data().duration().total_seconds() )
        {
        }

        double evaluate() override
        {
          double duration = base_duration * ( 1.0 + std::min( cp_max_spend, rupture->p()->current_cp() ) );
          return std::min( duration * 1.3, duration + rupture->td( rupture->target )->dots.rupture->remains().total_seconds() );
        }
      };

      return std::make_unique<new_duration_expr_t>( this );
    }

    return rogue_attack_t::create_expression( name );
  }

  void trigger_scent_of_blood()
  {
    if ( !p()->talent.assassination.scent_of_blood->ok() )
      return;

    const int current_stacks = p()->buffs.scent_of_blood->check();
    int desired_stacks = p()->get_active_dots( this->internal_id );
    if ( p()->active.deathmark.rupture )
    {
      desired_stacks += p()->get_active_dots( p()->active.deathmark.rupture->internal_id );
    }

    if ( current_stacks != desired_stacks )
    {
      p()->sim->print_log( "{} adjusting Scent of Blood stacks from {} to {}",
                           *p(), current_stacks, desired_stacks );
    }

    if ( desired_stacks < current_stacks )
    {
      p()->buffs.scent_of_blood->decrement( current_stacks - desired_stacks );
    }
    else if ( desired_stacks > current_stacks )
    {
      p()->buffs.scent_of_blood->increment( desired_stacks - current_stacks );
    }
  }
};

// Secret Technique =========================================================

struct secret_technique_t : public rogue_attack_t
{
  struct secret_technique_attack_t : public rogue_attack_t
  {
    secret_technique_attack_t( util::string_view name, rogue_t* p, const spell_data_t* s ) :
      rogue_attack_t( name, p, s )
    {
      aoe = -1;
      reduced_aoe_targets = p->talent.subtlety.secret_technique->effectN( 6 ).base_value();
    }

    double combo_point_da_multiplier( const action_state_t* state ) const override
    {
      return static_cast<double>( cast_state( state )->get_combo_points() );
    }

    double composite_target_multiplier( player_t* target ) const override
    {
      double m = rogue_attack_t::composite_target_multiplier( target );

      if ( target != this->target )
        m *= p()->talent.subtlety.secret_technique->effectN( 3 ).percent();

      return m;
    }
  };

  secret_technique_attack_t* player_attack;
  secret_technique_attack_t* clone_attack;

  secret_technique_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->talent.subtlety.secret_technique, options_str )
  {
    may_miss = false;

    player_attack = p->get_secondary_trigger_action<secret_technique_attack_t>(
      secondary_trigger::SECRET_TECHNIQUE, "secret_technique_player", p->spec.secret_technique_attack );
    clone_attack = p->get_secondary_trigger_action<secret_technique_attack_t>(
      secondary_trigger::SECRET_TECHNIQUE, "secret_technique_clones", p->spec.secret_technique_clone_attack );

    add_child( player_attack );
    add_child( clone_attack );
  }

  void init() override
  {
    rogue_attack_t::init();

    // BUG: Does not trigger alacrity, see https://github.com/SimCMinMax/WoW-BugTracker/issues/816
    if ( p()->bugs )
      affected_by.alacrity = false;
  }

  void execute() override
  {
    rogue_attack_t::execute();

    int cp = cast_state( execute_state )->get_combo_points();

    // Hit of the main char happens right on cast.
    player_attack->trigger_secondary_action( execute_state->target, cp );

    // The clones seem to hit 1s and 1.3s later (no time reference in spell data though)
    // Trigger tracking buff until first clone's damage
    p()->buffs.secret_technique->trigger( 1_s );
    clone_attack->trigger_secondary_action( execute_state->target, cp, 1_s );
    clone_attack->trigger_secondary_action( execute_state->target, cp, 1.3_s );
  }
};

// Shadow Blades ============================================================

struct shadow_blades_attack_t : public rogue_attack_t
{
  shadow_blades_attack_t( util::string_view name, rogue_t* p ) :
    rogue_attack_t( name, p, p->spec.shadow_blades_attack ) // DFALPHA TOCHECK Toxic Onslaught
  {
    may_dodge = may_block = may_parry = false;
    attack_power_mod.direct = 0;
  }

  void init() override
  {
    rogue_attack_t::init();
    snapshot_flags = update_flags = 0;
  }
};

struct shadow_blades_t : public rogue_spell_t
{
  timespan_t precombat_seconds;

  shadow_blades_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_spell_t( name, p, p->talent.subtlety.shadow_blades ),
    precombat_seconds( 0_s )
  {
    add_option( opt_timespan( "precombat_seconds", precombat_seconds ) );
    parse_options( options_str );

    harmful = false;
    school = SCHOOL_SHADOW;

    add_child( p->active.shadow_blades_attack );
  }

  void execute() override
  {
    rogue_spell_t::execute();

    // 2022-02-07 -- Updated to extend existing buffs on hard-casts in 9.2
    p()->buffs.shadow_blades->extend_duration_or_trigger();

    if ( precombat_seconds > 0_s && !p()->in_combat )
    {
      p()->cooldowns.shadow_blades->adjust( -precombat_seconds, false );
      p()->buffs.shadow_blades->extend_duration( p(), -precombat_seconds );
    }
  }
};

// Shadow Dance =============================================================

struct shadow_dance_t : public rogue_spell_t
{
  shadow_dance_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_spell_t( name, p, p->spell.shadow_dance, options_str )
  {
    harmful = false;
    dot_duration = timespan_t::zero(); // No need to have a tick here
  }

  void execute() override
  {
    rogue_spell_t::execute();
    p()->buffs.shadow_dance->trigger();
    trigger_master_of_shadows();

    if ( p()->talent.subtlety.the_first_dance->ok() )
    {
      trigger_combo_point_gain( as<int>( p()->talent.subtlety.the_first_dance->effectN( 1 ).base_value() ),
                                p()->gains.the_first_dance );
    }
  }

  bool ready() override
  {
    // Cannot dance during stealth. Vanish works.
    if ( p()->buffs.stealth->check() )
      return false;

    return rogue_spell_t::ready();
  }
};

// Shadowstep ===============================================================

struct shadowstep_t : public rogue_spell_t
{
  shadowstep_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_spell_t( name, p, p->spell.shadowstep, options_str )
  {
    harmful = false;
    dot_duration = timespan_t::zero();
    base_teleport_distance = data().max_range();
    movement_directionality = movement_direction_type::OMNI;
  }

  void execute() override
  {
    rogue_spell_t::execute();
    p()->buffs.shadowstep->trigger();
  }

  void update_ready( timespan_t cd_duration ) override
  {
    if ( p()->talent.assassination.intent_to_kill->ok() )
    {
      if ( td( target )->dots.garrote->is_ticking() )
      {
        if ( cd_duration < 0_ms )
        {
          cd_duration = cooldown->duration;
        }
          
        cd_duration *= ( 1.0 - p()->talent.assassination.intent_to_kill->effectN( 1 ).percent() );
      }
    }

    rogue_spell_t::update_ready( cd_duration );
  }
};

// Shadowstrike =============================================================

struct akaaris_shadowstrike_t : public rogue_attack_t
{
  akaaris_shadowstrike_t( util::string_view name, rogue_t* p ) :
    rogue_attack_t( name, p, p->find_spell( 345121 ) )
  {
    base_multiplier *= p->legendary.akaaris_soul_fragment->effectN( 2 ).percent();
  }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );

    trigger_perforated_veins( state );
    trigger_weaponmaster( state, p()->active.weaponmaster.akaaris_shadowstrike );
  }

  // 2021-12-10 - Logs appear to show this working as of 9.1.5
  bool procs_shadow_blades_damage() const override
  { return true; }
};

struct shadowstrike_t : public rogue_attack_t
{
  shadowstrike_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->spec.shadowstrike, options_str )
  {
  }

  void init() override
  {
    rogue_attack_t::init();

    if ( !is_secondary_action() )
    {
      if ( p()->active.weaponmaster.shadowstrike )
        add_child( p()->active.weaponmaster.shadowstrike );
      if ( p()->active.weaponmaster.akaaris_shadowstrike )
        add_child( p()->active.weaponmaster.akaaris_shadowstrike );
      if ( p()->active.akaaris_shadowstrike )
        add_child( p()->active.akaaris_shadowstrike );
    }
  }

  void execute() override
  {
    rogue_attack_t::execute();

    if ( p()->buffs.premeditation->up() )
    {
      if ( p()->buffs.slice_and_dice->check() )
      {
        trigger_combo_point_gain( as<int>( p()->talent.subtlety.premeditation->effectN( 2 ).base_value() ), p()->gains.premeditation );
      }

      timespan_t premed_duration = timespan_t::from_seconds( p()->talent.subtlety.premeditation->effectN( 1 ).base_value() );
      // Slice and Dice extension from Premeditation appears to be capped at 46.8s in any build, which equals 5CP duration with full pandemic.
      premed_duration = std::min( premed_duration, 46.8_s - p()->buffs.slice_and_dice->remains() );
      if ( premed_duration > timespan_t::zero() )
        p()->buffs.slice_and_dice->extend_duration_or_trigger( premed_duration );

      p()->buffs.premeditation->expire();
    }

    // 2021-08-30 -- Logs appear to show updated behavior of PV and The Rotten benefitting WM procs
    if ( p()->buffs.the_rotten->up() )
    {
      trigger_combo_point_gain( as<int>( p()->buffs.the_rotten->check_value() ), p()->gains.the_rotten );
      p()->buffs.the_rotten->expire( 1_ms );
    }

    if ( p()->talent.subtlety.inevitability->ok() )
    {
      timespan_t extend_duration = timespan_t::from_seconds( p()->talent.subtlety.inevitability->effectN( 2 ).base_value() / 10.0 );
      p()->buffs.symbols_of_death->extend_duration( p(), extend_duration );
    }
  }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );

    trigger_perforated_veins( state );
    trigger_weaponmaster( state, p()->active.weaponmaster.shadowstrike );

    // Appears to be applied after weaponmastered attacks.
    trigger_find_weakness( state );
    trigger_akaaris_soul_fragment( state );

    if ( state->result == RESULT_CRIT && p()->set_bonuses.t29_subtlety_4pc->ok() )
    {
      p()->buffs.t29_subtlety_4pc->trigger();
      p()->buffs.t29_subtlety_4pc_black_powder->trigger();
    }
  }

  double action_multiplier() const override
  {
    double m = rogue_attack_t::action_multiplier();

    if ( p()->stealthed( STEALTH_BASIC ) )
    {
      m *= 1.0 + p()->spec.shadowstrike_stealth_buff->effectN( 2 ).percent();
    }

    return m;
  }

  // TODO: Distance movement support with distance targeting, next to the target
  double composite_teleport_distance( const action_state_t* ) const override
  {
    if ( is_secondary_action() )
    {
      return 0;
    }

    if ( p()->stealthed( STEALTH_BASIC ) )
    {
      return range + p()->spec.shadowstrike_stealth_buff->effectN( 1 ).base_value();
    }

    return 0;
  }
};

// Black Powder =============================================================

struct black_powder_t: public rogue_attack_t
{
  struct black_powder_bonus_t : public rogue_attack_t
  {
    int last_cp;

    black_powder_bonus_t( util::string_view name, rogue_t* p ) :
      rogue_attack_t( name, p, p->spec.black_powder_shadow_attack ),
      last_cp( 1 )
    {
      callbacks = false; // 2021-07-19-- Does not appear to trigger normal procs
      aoe = -1;
      reduced_aoe_targets = p->talent.subtlety.black_powder->effectN( 4 ).base_value();

      if ( p->talent.subtlety.shadowed_finishers->ok() )
      {
        // Spell has the full damage coefficient and is modified via talent scripting
        base_multiplier *= p->talent.subtlety.shadowed_finishers->effectN( 1 ).percent();
      }
    }

    void reset() override
    {
      rogue_attack_t::reset();
      last_cp = 1;
    }

    double combo_point_da_multiplier( const action_state_t* ) const override
    {
      return as<double>( last_cp );
    }

    size_t available_targets( std::vector< player_t* >& tl ) const override
    {
      rogue_attack_t::available_targets( tl );

      // Can only hit targets with the Find Weakness debuff
      tl.erase( std::remove_if( tl.begin(), tl.end(), [ this ]( player_t* t ) {
        return !this->td( t )->debuffs.find_weakness->check(); } ), tl.end() );

      return tl.size();
    }

    void execute() override
    {
      // Invalidate target cache to force re-checking Find Weakness debuffs.
      // Don't attempt to execute this attack if it has no valid targets
      target_cache.is_valid = false;
      if ( !target_list().empty() )
      {
        rogue_attack_t::execute();
      }
    }
  };

  black_powder_bonus_t* bonus_attack;

  black_powder_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ):
    rogue_attack_t( name, p, p->talent.subtlety.black_powder, options_str ),
    bonus_attack( nullptr )
  {
    aoe = -1;
    reduced_aoe_targets = p->talent.subtlety.black_powder->effectN( 4 ).base_value();

    if ( p->talent.subtlety.shadowed_finishers->ok() )
    {
      bonus_attack = p->get_background_action<black_powder_bonus_t>( "black_powder_bonus" );
      add_child( bonus_attack );
    }
  }

  void execute() override
  {
    rogue_attack_t::execute();

    // Deeper Daggers triggers before bonus damage which makes it self-affecting.
    p()->buffs.deeper_daggers->trigger();

    // BUG: Finality BP seems to affect every instance of shadow damage due to, err, spaghetti with the bonus attack trigger order and travel time?
    // See https://github.com/SimCMinMax/WoW-BugTracker/issues/747
    bool triggered_finality = false;
    if ( p()->spec.finality_black_powder_buff->ok() && !p()->buffs.finality_black_powder->check() )
    {
      p()->buffs.finality_black_powder->trigger();
      triggered_finality = true;
    }

    if ( bonus_attack )
    {
      bonus_attack->last_cp = cast_state( execute_state )->get_combo_points();
      bonus_attack->set_target( execute_state->target );
      bonus_attack->execute();
    }

    // See bug above.
    if ( !triggered_finality )
      p()->buffs.finality_black_powder->expire();

    if ( p()->set_bonuses.t29_subtlety_2pc->ok() )
    {
      p()->buffs.t29_subtlety_2pc->expire();
      p()->buffs.t29_subtlety_2pc->trigger( cast_state( execute_state )->get_combo_points() );
    }
  }

  bool procs_poison() const override
  { return true; }

  std::unique_ptr<expr_t> create_expression( util::string_view name_str ) override
  {
    if ( util::str_compare_ci( name_str, "fw_targets" ) )
    {
      return make_fn_expr( name_str, [ this ]() {
        auto count = range::count_if( rogue_attack_t::target_list(), [ this ]( player_t* t ) {
          return this->td( t )->debuffs.find_weakness->check();
        } );
        return count;
      } );
    }

    return rogue_attack_t::create_expression( name_str );
  }
};

// Shuriken Storm ===========================================================

struct shuriken_storm_t: public rogue_attack_t
{
  shuriken_storm_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ):
    rogue_attack_t( name, p, p->spec.shuriken_storm, options_str )
  {
    energize_type = action_energize::PER_HIT;
    energize_resource = RESOURCE_COMBO_POINT;
    energize_amount = 1;
    ap_type = attack_power_type::WEAPON_BOTH;
    // 2021-04-22- Not in the whitelist but confirmed as working in-game
    affected_by.shadow_blades_cp = true;

    aoe = -1;
    reduced_aoe_targets = data().effectN( 4 ).base_value();
  }

  double composite_crit_chance() const override
  {
    double c = rogue_attack_t::composite_crit_chance();    
    c += p()->buffs.silent_storm->stack_value();
    return c;
  }

  /* DFALPHA -- This is currently bugged in-game and is a crit multiplier as below
  *             Likely not intended but leaving this code just in case it makes live
  double composite_crit_chance_multiplier() const override
  {
    double cm = rogue_attack_t::composite_crit_chance_multiplier();
    cm *= 1.0 + p()->buffs.silent_storm->stack_value();
    return cm;
  }
  */

  void execute() override
  {
    rogue_attack_t::execute();

    // TOCHECK DFALPHA -- Shuriken Tornado secondary actions do not expire this on beta
    if ( !is_secondary_action() )
    {
      p()->buffs.silent_storm->expire();
    }
  }

  void impact(action_state_t* state) override
  {
    rogue_attack_t::impact( state );

    if ( state->result == RESULT_CRIT && p()->talent.subtlety.improved_shuriken_storm->ok() )
    {
      timespan_t duration = timespan_t::from_seconds( p()->talent.subtlety.improved_shuriken_storm->effectN( 1 ).base_value() );
      trigger_find_weakness( state, duration );
    }

    if ( state->result == RESULT_CRIT && p()->set_bonuses.t29_subtlety_4pc->ok() )
    {
      p()->buffs.t29_subtlety_4pc->trigger();
      p()->buffs.t29_subtlety_4pc_black_powder->trigger();
    }
  }

  // 2021-07-12-- Shuriken Tornado triggers the damage directly without a cast, so cast triggers don't happen
  bool procs_poison() const override
  { return secondary_trigger_type != secondary_trigger::SHURIKEN_TORNADO; }
};

// Shuriken Tornado =========================================================

struct shuriken_tornado_t : public rogue_spell_t
{
  shuriken_tornado_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_spell_t( name, p, p->talent.subtlety.shuriken_tornado, options_str )
  {
    dot_duration = timespan_t::zero();
    aoe = -1;

    // Trigger action is created in the buff, but it can't be parented then, just look it up here
    action_t* trigger_action = p->find_secondary_trigger_action<shuriken_storm_t>( secondary_trigger::SHURIKEN_TORNADO );
    if ( trigger_action )
    {
      add_child( trigger_action );
    }
  }

  void execute() override
  {
    rogue_spell_t::execute();
    p()->buffs.shuriken_tornado->trigger();
  }
};

// Shuriken Toss ============================================================

struct shuriken_toss_t : public rogue_attack_t
{
  shuriken_toss_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->spec.shuriken_toss, options_str )
  {
    ap_type = attack_power_type::WEAPON_BOTH;
  }
};

// Sinister Strike ==========================================================

struct sinister_strike_t : public rogue_attack_t
{
  struct triple_threat_t : public rogue_attack_t
  {
    triple_threat_t( util::string_view name, rogue_t* p ) :
      rogue_attack_t( name, p, p->spec.triple_threat_attack )
    {
    }

    void execute() override
    {
      rogue_attack_t::execute();
      trigger_guile_charm( execute_state );
      p()->active.triple_threat_mh->trigger_secondary_action( execute_state->target );
    }

    bool procs_main_gauche() const override
    { return true; }

    bool procs_blade_flurry() const override
    { return true; }
  };

  struct sinister_strike_extra_attack_t : public rogue_attack_t
  {
    double triple_threat_chance;

    sinister_strike_extra_attack_t( util::string_view name, rogue_t* p ) :
      rogue_attack_t( name, p, p->spec.sinister_strike_extra_attack ),
      triple_threat_chance( 0.0 )
    {
      energize_type = action_energize::ON_HIT;
      energize_resource = RESOURCE_COMBO_POINT;

      triple_threat_chance = ( p->talent.outlaw.triple_threat->ok() ?
                               p->talent.outlaw.triple_threat->effectN( 1 ).percent() :
                               p->conduit.triple_threat.percent() );
    }

    double composite_energize_amount( const action_state_t* ) const override
    {
      // CP generation is not in the spell data and the extra SS procs seem script-driven
      // Triple Threat procs of this spell don't give combo points, however
      return ( secondary_trigger_type == secondary_trigger::SINISTER_STRIKE ) ? 1 : 0;
    }

    void execute() override
    {
      rogue_attack_t::execute();
      trigger_guile_charm( execute_state );

      // Triple Threat procs do not appear to be able to chain-proc based on testing
      if ( secondary_trigger_type == secondary_trigger::SINISTER_STRIKE && p()->active.triple_threat_oh &&
           p()->rng().roll( triple_threat_chance ) )
      {
        p()->active.triple_threat_oh->trigger_secondary_action( execute_state->target, 300_ms );
      }
    }

    bool procs_main_gauche() const override
    { return true; }

    bool procs_blade_flurry() const override
    { return true; }
  };

  sinister_strike_extra_attack_t* extra_attack;

  sinister_strike_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->spec.sinister_strike, options_str )
  {
    extra_attack = p->get_secondary_trigger_action<sinister_strike_extra_attack_t>(
      secondary_trigger::SINISTER_STRIKE, "sinister_strike_extra_attack" );
  }

  void init() override
  {
    rogue_attack_t::init();

    if ( !is_secondary_action() )
    {
      add_child( extra_attack );
      if ( p()->active.triple_threat_mh )
        add_child( p()->active.triple_threat_mh );
      if ( p()->active.triple_threat_oh )
        add_child( p()->active.triple_threat_oh );
    }
  }

  void execute() override
  {
    rogue_attack_t::execute();
    trigger_guile_charm( execute_state );
    trigger_opportunity( execute_state, extra_attack );
  }

  bool procs_main_gauche() const override
  { return true; }

  bool procs_blade_flurry() const override
  { return true; }
};

// Slice and Dice ===========================================================

struct slice_and_dice_t : public rogue_spell_t
{
  timespan_t precombat_seconds;

  slice_and_dice_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_spell_t( name, p, p->spell.slice_and_dice ),
    precombat_seconds( 0_s )
  {
    add_option( opt_timespan( "precombat_seconds", precombat_seconds ) );
    parse_options( options_str );

    harmful = false;
    dot_duration = timespan_t::zero();
  }

  timespan_t get_triggered_duration( int cp )
  {
    return ( cp + 1 ) * p()->buffs.slice_and_dice->data().duration();
  }

  void execute() override
  {
    rogue_spell_t::execute();

    trigger_restless_blades( execute_state );

    int cp = cast_state( execute_state )->get_combo_points();
    timespan_t snd_duration = get_triggered_duration( cp );

    if ( precombat_seconds > 0_s && !p()->in_combat )
      snd_duration -= precombat_seconds;

    if ( p()->talent.outlaw.swift_slasher->ok() )
    {
      const double buffed_value = ( p()->buffs.slice_and_dice->default_value +
                                    p()->talent.outlaw.swift_slasher->effectN( 1 ).percent() * cp );
      p()->buffs.slice_and_dice->trigger( -1, buffed_value, -1.0, snd_duration );
    }
    else
    {
      p()->buffs.slice_and_dice->trigger( snd_duration );
    }

    // Grand melee extension goes on top of SnD buff application.
    trigger_grand_melee( execute_state );
  }

  bool ready() override
  {
    // Grand Melee prevents refreshing if there would be a reduction in the post-pandemic buff duration
    if ( p()->buffs.slice_and_dice->remains() > get_triggered_duration( as<int>( p()->current_effective_cp( false ) ) ) * 1.3 )
      return false;

    return rogue_spell_t::ready();
  }

  std::unique_ptr<expr_t> create_expression( util::string_view name_str ) override
  {
    if ( util::str_compare_ci( name_str, "refreshable" ) )
      return make_fn_expr( name_str, [ this ]() {
        return p()->buffs.slice_and_dice->remains() < get_triggered_duration( as<int>( p()->current_effective_cp( false ) ) ) * 0.3;
      } );

    return rogue_spell_t::create_expression( name_str );
  }
};

// Sprint ===================================================================

struct sprint_t : public rogue_spell_t
{
  sprint_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ):
    rogue_spell_t( name, p, p->spell.sprint, options_str )
  {
    harmful = callbacks = false;
    cooldown = p->cooldowns.sprint;
  }

  void execute() override
  {
    rogue_spell_t::execute();
    p()->buffs.sprint->trigger();
  }
};

// Symbols of Death =========================================================

struct symbols_of_death_t : public rogue_spell_t
{
  symbols_of_death_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_spell_t( name, p, p->spec.symbols_of_death, options_str )
  {
    harmful = callbacks = false;
    dot_duration = timespan_t::zero();
  }

  void execute() override
  {
    rogue_spell_t::execute();

    p()->buffs.symbols_of_death->trigger();
    p()->buffs.the_rotten->trigger();
  }
};

// Shiv =====================================================================

struct shiv_t : public rogue_attack_t
{
  shiv_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->talent.rogue.shiv, options_str )
  {
  }

  void impact( action_state_t* s ) override
  {
    rogue_attack_t::impact( s );

    if ( result_is_hit( s->result ) && p()->talent.assassination.improved_shiv->ok() )
    {
      td( s->target )->debuffs.shiv->trigger();
    }
  }

  bool procs_fatal_flourish() const override
  { return true; }

  bool procs_blade_flurry() const override
  { return true; }
};

// Vanish ===================================================================

struct vanish_t : public rogue_spell_t
{
  vanish_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_spell_t( name, p, p->spell.vanish, options_str )
  {
    harmful = false;
  }

  void execute() override
  {
    rogue_spell_t::execute();

    p()->buffs.vanish->trigger();
    trigger_master_of_shadows();
  }

  bool ready() override
  {
    if ( p()->buffs.vanish->check() )
      return false;

    return rogue_spell_t::ready();
  }
};

// Stealth ==================================================================

struct stealth_t : public rogue_spell_t
{
  stealth_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_spell_t( name, p, p->spell.stealth, options_str )
  {
    harmful = false;
  }

  void execute() override
  {
    rogue_spell_t::execute();
    p()->buffs.stealth->trigger();
    trigger_master_of_shadows();
  }

  bool ready() override
  {
    if ( p() -> stealthed( STEALTH_BASIC | STEALTH_ROGUE ) )
      return false;

    if ( ! p() -> in_combat )
      return true;

    // HAX: Allow restealth for DungeonSlice against non-"boss" targets because Shadowmeld drops combat against trash.
    if ( p()->sim->fight_style == FIGHT_STYLE_DUNGEON_SLICE && p()->player_t::buffs.shadowmeld->check() && target->type == ENEMY_ADD )
      return true;

    if ( !p()->restealth_allowed )
      return false;

    return rogue_spell_t::ready();
  }
};

// Kidney Shot ==============================================================

struct kidney_shot_t : public rogue_attack_t
{
  struct internal_bleeding_t : public rogue_attack_t
  {
    internal_bleeding_t( util::string_view name, rogue_t* p ) :
      rogue_attack_t( name, p, p->spec.internal_bleeding_debuff ) // DFALPHA -- Check scaling with talent points
    {
    }

    timespan_t composite_dot_duration( const action_state_t* s ) const override
    {
      timespan_t duration = rogue_attack_t::composite_dot_duration( s );
      duration *= 1.0 / cast_state( s )->get_exsanguinated_rate();
      return duration;
    }

    double composite_persistent_multiplier( const action_state_t* state ) const override
    {
      double m = rogue_attack_t::composite_persistent_multiplier( state );
      m *= cast_state( state )->get_combo_points();
      return m;
    }

    void tick( dot_t* d ) override
    {
      rogue_attack_t::tick( d );
      trigger_venomous_wounds( d->state );
    }
  };

  internal_bleeding_t* internal_bleeding;

  kidney_shot_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->spell.kidney_shot, options_str ),
    internal_bleeding( nullptr )
  {
    if ( p->talent.assassination.internal_bleeding->ok() )
    {
      internal_bleeding = p->get_secondary_trigger_action<internal_bleeding_t>(
        secondary_trigger::INTERNAL_BLEEDING, "internal_bleeding" );
      add_child( internal_bleeding );
    }
  }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );

    trigger_prey_on_the_weak( state );

    if ( !state->target->is_boss() && internal_bleeding )
    {
      internal_bleeding->trigger_secondary_action( state->target, cast_state( state )->get_combo_points() );
    }
  }
};

// Cheap Shot ===============================================================

struct cheap_shot_t : public rogue_attack_t
{
  cheap_shot_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->spell.cheap_shot, options_str )
  {
  }

  double cost() const override
  {
    double c = rogue_attack_t::cost();
    if ( p()->buffs.shot_in_the_dark->up() )
      return 0.0;
    return c;
  }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );
    trigger_prey_on_the_weak( state );
    trigger_find_weakness( state );
    trigger_akaaris_soul_fragment( state );
  }
};

// Poison Bomb ==============================================================

struct poison_bomb_t : public rogue_attack_t
{
  poison_bomb_t( util::string_view name, rogue_t* p ) :
    rogue_attack_t( name, p, p->spec.poison_bomb_damage )
  {
    aoe = -1;
  }
};

// Vicious Venoms ===========================================================

struct vicious_venoms_t : public rogue_attack_t
{
  vicious_venoms_t( util::string_view name, rogue_t* p, const spell_data_t* s ) :
    rogue_attack_t( name, p, s )
  {
    // Appears to be overridden by a scripted multiplier even though the base damage is identical
    base_multiplier *= p->talent.assassination.vicious_venoms->effectN( 1 ).percent();
  }

  bool procs_poison() const override
  { return false; }
};

// Poisoned Knife ===========================================================

struct poisoned_knife_t : public rogue_attack_t
{
  poisoned_knife_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->spec.poisoned_knife, options_str )
  {
  }

  double composite_poison_flat_modifier( const action_state_t* ) const override
  { return 1.0; }
};

// Thistle Tea ==============================================================

struct thistle_tea_t : public rogue_spell_t
{
  double precombat_seconds;

  thistle_tea_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_spell_t( name, p, p->talent.rogue.thistle_tea ),
    precombat_seconds( 0.0 )
  {
    add_option( opt_float( "precombat_seconds", precombat_seconds ) );
    parse_options( options_str );

    harmful = false;
    energize_type = action_energize::ON_CAST;
  }

  void execute() override
  {
    rogue_spell_t::execute();

    if ( precombat_seconds && !p()->in_combat )
    {
      p()->cooldowns.thistle_tea->adjust( -timespan_t::from_seconds( precombat_seconds ), false );
    }

    p()->buffs.thistle_tea->trigger();
  }


  bool ready() override
  {
    // Cannot use the ability just for the buff if at full energy
    if ( p()->resources.current[ RESOURCE_ENERGY ] >= p()->resources.max[ RESOURCE_ENERGY ] )
      return false;

    return rogue_spell_t::ready();
  }
};

// Keep it Rolling ==========================================================

struct keep_it_rolling_t : public rogue_spell_t
{
  keep_it_rolling_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_spell_t( name, p, p->talent.outlaw.keep_it_rolling, options_str )
  {
    harmful = false;
  }

  void execute() override
  {
    rogue_spell_t::execute();
    trigger_keep_it_rolling();
  }
};

// ==========================================================================
// Covenant Abilities
// ==========================================================================

// Echoing Reprimand ========================================================

struct echoing_reprimand_t : public rogue_attack_t
{
  double random_min, random_max;

  echoing_reprimand_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->spell.echoing_reprimand, options_str ),
    random_min( 0 ), random_max( 3 ) // Randomizes between 2CP and 4CP buffs
  {
  }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );

    if ( result_is_hit( state->result ) )
    {
      // Casting Echoing Reprimand cancels all existing buffs. Relevant for CDR trait in AoE.
      for ( buff_t* b : p()->buffs.echoing_reprimand )
        b->expire();

      if ( p()->legendary.resounding_clarity->ok() || p()->talent.rogue.resounding_clarity->ok() )
      {
        for ( buff_t* b : p()->buffs.echoing_reprimand )
          b->trigger();
      }
      else
      {
        unsigned buff_idx = static_cast<int>( rng().range( random_min, random_max ) );
        p()->buffs.echoing_reprimand[ buff_idx ]->trigger();
      }

      // TOCHECK -- Due to beta behavior never removed, Echoing Reprimand can trigger FW from Stealth
      if ( p()->bugs && p()->stealthed( STEALTH_BASIC | STEALTH_SHADOWDANCE ) )
      {
        trigger_find_weakness( state );
      }
    }
  }

  bool procs_blade_flurry() const override
  { return true; }
};

// Flagellation =============================================================

struct flagellation_damage_t : public rogue_attack_t
{
  buff_t* debuff;

  flagellation_damage_t( util::string_view name, rogue_t* p ) :
    rogue_attack_t( name, p, p->spec.flagellation_damage ),
    debuff( nullptr )
  {
    dual = true;
  }

  double combo_point_da_multiplier( const action_state_t* s ) const override
  {
    return as<double>( cast_state( s )->get_combo_points() );
  }
};

struct flagellation_covenant_t : public rogue_attack_t
{
  int initial_lashes;

  flagellation_covenant_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->covenant.flagellation, options_str ),
    initial_lashes()
  {
    dot_duration = timespan_t::zero();
    // Manually setting to false because the spell is still in the Shadow Blades whitelist.
    affected_by.shadow_blades_cp = false;

    if ( p->active.flagellation )
    {
      add_child( p->active.flagellation );
    }

    if ( p->conduit.lashing_scars->ok() )
    {
      initial_lashes = as<int>( p->conduit.lashing_scars->effectN( 2 ).base_value() );
    }
  }

  void execute() override
  {
    rogue_attack_t::execute();

    p()->active.flagellation->debuff = td( execute_state->target )->debuffs.flagellation;
    p()->active.flagellation->debuff->trigger();

    p()->buffs.flagellation->trigger();
    if ( p()->conduit.lashing_scars->ok() )
    {
      // Additional lashes via the conduit seem to just cause an extra lash event as if an X CP Finisher has been cast.
      // Only difference to finishers is the buff stacks are delayed and happen with the lash whereas for finishers they happen instantly.
      make_event( *sim, 0.75_s, [ this ]( )
      {
        p()->buffs.flagellation->trigger( initial_lashes );
        p()->active.flagellation->trigger_secondary_action( execute_state->target, initial_lashes );
      } );
    }
  }

  // 2022-07-02 -- Initial damage procs Blade Flurry, lashes do not
  bool procs_blade_flurry() const override
  { return true; }
};

struct flagellation_t : public rogue_attack_t
{
  flagellation_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->talent.subtlety.flagellation, options_str )
  {
    dot_duration = timespan_t::zero();

    if ( p->active.flagellation )
    {
      add_child( p->active.flagellation );
    }
  }

  void execute() override
  {
    rogue_attack_t::execute();

    p()->active.flagellation->debuff = td( execute_state->target )->debuffs.flagellation;
    p()->active.flagellation->debuff->trigger();

    p()->buffs.flagellation->trigger();
  }
};

// Sepsis ===================================================================

struct sepsis_covenant_t : public rogue_attack_t
{
  struct sepsis_expire_damage_t : public rogue_attack_t
  {
    sepsis_expire_damage_t( util::string_view name, rogue_t* p ) :
      rogue_attack_t( name, p, p->spell.sepsis_expire_damage )
    {
      dual = true;
    }

    void impact( action_state_t* state ) override
    {
      // 2020-12-30- Due to flagging as a generator, the final hit can trigger Seal Fate
      rogue_attack_t::impact( state );
      trigger_seal_fate( state );
    }

    // 2021-04-22-- Confirmed as working in-game
    bool procs_shadow_blades_damage() const override
    { return true; }
  };

  sepsis_expire_damage_t* sepsis_expire_damage;

  sepsis_covenant_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->covenant.sepsis, options_str )
  {
    affected_by.broadside_cp = true; // 2021-04-22 -- Not in the whitelist but confirmed as working in-game
    sepsis_expire_damage = p->get_background_action<sepsis_expire_damage_t>( "sepsis_expire_damage" );
    sepsis_expire_damage->stats = stats;
  }

  double composite_ta_multiplier( const action_state_t* state ) const override
  {
    double m = rogue_attack_t::composite_ta_multiplier( state );

    if ( p()->conduit.septic_shock.ok() )
    {
      const dot_t* dot = td( state->target )->dots.sepsis;
      const double reduction = ( dot->current_tick - 1 ) * p()->conduit.septic_shock->effectN( 2 ).percent();
      const double multiplier = p()->conduit.septic_shock.percent() * std::max( 1.0 - reduction , 0.0 );
      m *= 1.0 + multiplier;
    }

    return m;
  }

  void tick( dot_t* d ) override
  {
    // 2022-02-10 -- Logs appear to show the buff portion triggers prior to the final tick and damage
    //               However, Vendetta happens after the damage event and must be delayed
    if ( d->remains() == timespan_t::zero() )
    {
      p()->trigger_toxic_onslaught( d->target );
    }
    rogue_attack_t::tick( d );
  }

  void last_tick( dot_t* d ) override
  {
    rogue_attack_t::last_tick( d );
    sepsis_expire_damage->set_target( d->target );
    sepsis_expire_damage->execute();
    p()->buffs.sepsis->trigger();
  }

  bool snapshots_nightstalker() const override
  { return false; }
};

struct sepsis_t : public rogue_attack_t
{
  struct sepsis_expire_damage_t : public rogue_attack_t
  {
    sepsis_expire_damage_t( util::string_view name, rogue_t* p ) :
      rogue_attack_t( name, p, p->spell.sepsis_expire_damage )
    {
      dual = true;
    }

    void impact( action_state_t* state ) override
    {
      // TOCHECK DFALPHA
      // 2020-12-30- Due to flagging as a generator, the final hit can trigger Seal Fate
      rogue_attack_t::impact( state );
      trigger_seal_fate( state );
    }

    // TOCHECK DFALPHA
    // 2021-04-22-- Confirmed as working in-game
    bool procs_shadow_blades_damage() const override
    { return true; }
  };

  sepsis_expire_damage_t* sepsis_expire_damage;

  sepsis_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->talent.shared.sepsis, options_str )
  {
    affected_by.broadside_cp = true; // TOCHECK DFALPHA 2021-04-22 -- Not in the whitelist but confirmed as working in-game
    sepsis_expire_damage = p->get_background_action<sepsis_expire_damage_t>( "sepsis_expire_damage" );
    sepsis_expire_damage->stats = stats;
  }

  double composite_ta_multiplier( const action_state_t* state ) const override
  {
    double m = rogue_attack_t::composite_ta_multiplier( state );

    // TOCHECK DFALPHA
    if ( p()->conduit.septic_shock.ok() )
    {
      const dot_t* dot = td( state->target )->dots.sepsis;
      const double reduction = ( dot->current_tick - 1 ) * p()->conduit.septic_shock->effectN( 2 ).percent();
      const double multiplier = p()->conduit.septic_shock.percent() * std::max( 1.0 - reduction, 0.0 );
      m *= 1.0 + multiplier;
    }

    return m;
  }

  void tick( dot_t* d ) override
  {
    // 2022-02-10 -- Logs appear to show the buff portion triggers prior to the final tick and damage
    //               However, Vendetta happens after the damage event and must be delayed
    if ( d->remains() == timespan_t::zero() )
    {
      p()->trigger_toxic_onslaught( d->target );
    }
    rogue_attack_t::tick( d );
  }

  void last_tick( dot_t* d ) override
  {
    rogue_attack_t::last_tick( d );
    sepsis_expire_damage->set_target( d->target );
    sepsis_expire_damage->execute();
    p()->buffs.sepsis->trigger();
  }

  bool snapshots_nightstalker() const override
  { return false; }
};

// Serrated Bone Spike ======================================================

struct serrated_bone_spike_covenant_t : public rogue_attack_t
{
  struct serrated_bone_spike_dot_t : public rogue_attack_t
  {
    struct sudden_fractures_t : public rogue_attack_t
    {
      sudden_fractures_t( util::string_view name, rogue_t* p ) :
        rogue_attack_t( name, p, p->find_spell( 341277 ) )
      {
      }

      bool procs_poison() const override
      { return false; }
    };

    sudden_fractures_t* sudden_fractures;

    serrated_bone_spike_dot_t( util::string_view name, rogue_t* p ) :
      rogue_attack_t( name, p, p->covenant.serrated_bone_spike->effectN( 2 ).trigger() ),
      sudden_fractures( nullptr )
    {
      aoe = 0; // Technically affected by Deathspike, but interferes with our triggering logic
      hasted_ticks = true; // 2021-03-12 - Bone spike dot is hasted, despite not being flagged as such
      affected_by.zoldyck_insignia = true; // 2021-02-13 - Logs show that the SBS DoT is affected by Zoldyck
      dot_duration = timespan_t::from_seconds( sim->expected_max_time() * 3 );

      if ( p->conduit.sudden_fractures.ok() )
      {
        sudden_fractures = p->get_background_action<sudden_fractures_t>( "sudden_fractures" );
      }
    }

    void tick( dot_t* d ) override
    {
      rogue_attack_t::tick( d );

      if ( sudden_fractures && p()->rng().roll( p()->conduit.sudden_fractures.percent() ) )
      {
        sudden_fractures->set_target( d->target );
        sudden_fractures->execute();
      }
    }

    // 2021-03-28 -- Testing shows that Nightstalker works if you are very close to the target's hitbox
    //               This works on both the initial hit and also the DoT, until it is applied again
    bool snapshots_nightstalker() const override
    { return p()->bugs; }

    // 2021-07-05 -- Confirmed as working in-game, although not on Sudden Fractures damage
    bool procs_shadow_blades_damage() const override
    { return true; }

    bool procs_poison() const override
    { return false; }
  };

  int base_impact_cp;
  serrated_bone_spike_dot_t* serrated_bone_spike_dot;

  serrated_bone_spike_covenant_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->covenant.serrated_bone_spike, options_str )
  {
    // Combo Point generation is in a secondary spell due to scripting logic
    // 2021-07-09 -- Not in the whitelist but confirmed as working in-game as of 9.1 patch notes
    affected_by.shadow_blades_cp = true;
    affected_by.broadside_cp = true;
    energize_type = action_energize::ON_HIT;
    energize_resource = RESOURCE_COMBO_POINT;
    energize_amount = 0.0; // Not done on execute but we keep the ON_HIT energize for Dreadblades/Broadside/Shadow Blades
    base_impact_cp = as<int>( p->spec.serrated_bone_spike_energize->effectN( 1 ).base_value() );

    serrated_bone_spike_dot = p->get_background_action<serrated_bone_spike_dot_t>( "serrated_bone_spike_dot" );

    add_child( serrated_bone_spike_dot );
    if ( serrated_bone_spike_dot->sudden_fractures )
    {
      add_child( serrated_bone_spike_dot->sudden_fractures );
    }
  }

  dot_t* get_dot( player_t* t ) override
  {
    if ( !t ) t = target;
    if ( !t ) return nullptr;
    return td( t )->dots.serrated_bone_spike;
  }

  double generate_cp() const override
  {
    double cp = rogue_attack_t::generate_cp();

    cp += base_impact_cp;
    cp += p()->get_active_dots( serrated_bone_spike_dot->internal_id );
    cp += !td( target )->dots.serrated_bone_spike->is_ticking();

    return cp;
  }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );

    // 2021-03-04 -- 9.0.5: Bonus CP gain now **supposed to** include the primary target DoT even on first activation
    unsigned active_dots = p()->get_active_dots( serrated_bone_spike_dot->internal_id );

    // BUG, see https://github.com/SimCMinMax/WoW-BugTracker/issues/823
    // Race condition on when the spikes are counted. We just make it 50% chance.
    bool count_after = p()->bugs ? rng().roll( 0.5 ) : true;

    auto tdata = td( state->target );
    if ( !tdata->dots.serrated_bone_spike->is_ticking() && count_after )
    {
      active_dots++;
    }

    // Due to the Vendetta 4pc, we need to re-apply the DoT to recalculate the haste snapshot
    serrated_bone_spike_dot->set_target( state->target );
    serrated_bone_spike_dot->execute();
 
    // 2022-01-26 -- PTR shows this happens on impact but only for the primary target
    //               Deathspiked targets do not generate CP directly
    if ( state->chain_target == 0 )
    {
      trigger_combo_point_gain( base_impact_cp + active_dots, p()->gains.serrated_bone_spike );
    }
  }

  timespan_t travel_time() const override
  {
    // 2021-03-28 -- Testing shows that Nightstalker works if you are very close to the target's hitbox
    // Assume if the player is playing Nightstalker they are getting inside the hitbox to reduce travel time
    if ( p()->bugs && p()->talent.rogue.nightstalker->ok() && p()->stealthed( STEALTH_BASIC | STEALTH_SHADOWDANCE ) )
      return timespan_t::zero();

    return rogue_attack_t::travel_time();
  }

  bool procs_blade_flurry() const override
  { return true; }

  // 2021-07-05 -- Confirmed as working in-game
  bool procs_shadow_blades_damage() const override
  { return true; }
};

struct serrated_bone_spike_t : public rogue_attack_t
{
  struct serrated_bone_spike_dot_t : public rogue_attack_t
  {
    serrated_bone_spike_dot_t( util::string_view name, rogue_t* p ) :
      rogue_attack_t( name, p, p->talent.assassination.serrated_bone_spike->effectN( 2 ).trigger() )
    {
      aoe = 0; // Technically affected by Deathspike, but interferes with our triggering logic
      hasted_ticks = true; // TOCHECK DFALPHA -- 2021-03-12 - Bone spike dot is hasted, despite not being flagged as such
      affected_by.zoldyck_insignia = true; // TOCHECK DFALPHA -- 2021-02-13 - Logs show that the SBS DoT is affected by Zoldyck
      dot_duration = timespan_t::from_seconds( sim->expected_max_time() * 3 );
    }

    // 2021-03-28 -- Testing shows that Nightstalker works if you are very close to the target's hitbox
    //               This works on both the initial hit and also the DoT, until it is applied again
    bool snapshots_nightstalker() const override
    { return p()->bugs; }

    bool procs_poison() const override
    { return false; }
  };

  int base_impact_cp;
  serrated_bone_spike_dot_t* serrated_bone_spike_dot;

  serrated_bone_spike_t( util::string_view name, rogue_t* p, util::string_view options_str = {} ) :
    rogue_attack_t( name, p, p->talent.assassination.serrated_bone_spike, options_str )
  {
    // Combo Point generation is in a secondary spell due to scripting logic
    energize_type = action_energize::ON_HIT;
    energize_resource = RESOURCE_COMBO_POINT;
    energize_amount = 0.0;
    base_impact_cp = as<int>( p->spec.serrated_bone_spike_energize->effectN( 1 ).base_value() );

    serrated_bone_spike_dot = p->get_background_action<serrated_bone_spike_dot_t>( "serrated_bone_spike_dot" );

    add_child( serrated_bone_spike_dot );
  }

  dot_t* get_dot( player_t* t ) override
  {
    if ( !t ) t = target;
    if ( !t ) return nullptr;
    return td( t )->dots.serrated_bone_spike;
  }

  double generate_cp() const override
  {
    double cp = rogue_attack_t::generate_cp();

    cp += base_impact_cp;
    cp += p()->get_active_dots( serrated_bone_spike_dot->internal_id );
    cp += !td( target )->dots.serrated_bone_spike->is_ticking();

    return cp;
  }

  void impact( action_state_t* state ) override
  {
    rogue_attack_t::impact( state );

    // 2021-03-04 -- 9.0.5: Bonus CP gain now **supposed to** include the primary target DoT even on first activation
    unsigned active_dots = p()->get_active_dots( serrated_bone_spike_dot->internal_id );

    // BUG, see https://github.com/SimCMinMax/WoW-BugTracker/issues/823
    // Race condition on when the spikes are counted. We just make it 50% chance.
    bool count_after = p()->bugs ? rng().roll( 0.5 ) : true;

    auto tdata = td( state->target );
    if ( !tdata->dots.serrated_bone_spike->is_ticking() && count_after )
    {
      active_dots++;
    }

    // Due to the Vendetta 4pc, we need to re-apply the DoT to recalculate the haste snapshot
    serrated_bone_spike_dot->set_target( state->target );
    serrated_bone_spike_dot->execute();
 
    // 2022-01-26 -- PTR shows this happens on impact but only for the primary target
    //               Deathspiked targets do not generate CP directly
    if ( state->chain_target == 0 )
    {
      trigger_combo_point_gain( base_impact_cp + active_dots, p()->gains.serrated_bone_spike );
    }
  }

  timespan_t travel_time() const override
  {
    // 2021-03-28 -- Testing shows that Nightstalker works if you are very close to the target's hitbox
    // Assume if the player is playing Nightstalker they are getting inside the hitbox to reduce travel time
    if ( p()->bugs && p()->talent.rogue.nightstalker->ok() && p()->stealthed( STEALTH_BASIC | STEALTH_SHADOWDANCE ) )
      return timespan_t::zero();

    return rogue_attack_t::travel_time();
  }
};

// ==========================================================================
// Cancel AutoAttack
// ==========================================================================

struct cancel_autoattack_t : public action_t
{
  rogue_t* rogue;
  cancel_autoattack_t( rogue_t* rogue_, util::string_view options_str = {} ) :
    action_t( ACTION_OTHER, "cancel_autoattack", rogue_ ),
    rogue( rogue_ )
  {
    parse_options( options_str );

    trigger_gcd = timespan_t::zero();
  }

  result_e calculate_result( action_state_t* ) const override
  { return RESULT_HIT; }

  block_result_e calculate_block_result( action_state_t* ) const override
  { return BLOCK_RESULT_UNBLOCKED; }

  void execute() override
  {
    action_t::execute();
    rogue->cancel_auto_attacks();
  }

  bool ready() override
  {
    if ( ( rogue->main_hand_attack && rogue->main_hand_attack->execute_event ) ||
         ( rogue->off_hand_attack && rogue->off_hand_attack->execute_event ) )
    {
      return action_t::ready();
    }

    return false;
  }
};

// ==========================================================================
// Experimental weapon swapping
// ==========================================================================

struct weapon_swap_t : public action_t
{
  enum swap_slot_e
  {
    SWAP_MAIN_HAND,
    SWAP_OFF_HAND,
    SWAP_BOTH
  };

  std::string slot_str, swap_to_str;

  swap_slot_e swap_type;
  current_weapon_e swap_to_type;
  rogue_t* rogue;

  weapon_swap_t( rogue_t* rogue_, util::string_view options_str ) :
    action_t( ACTION_OTHER, "weapon_swap", rogue_ ),
    swap_type( SWAP_MAIN_HAND ), swap_to_type( WEAPON_SECONDARY ),
    rogue( rogue_ )
  {
    may_miss = may_crit = may_dodge = may_parry = may_glance = callbacks = harmful = false;

    add_option( opt_string( "slot", slot_str ) );
    add_option( opt_string( "swap_to", swap_to_str ) );

    parse_options( options_str );

    if ( slot_str.empty() )
    {
      background = true;
    }
    else if ( util::str_compare_ci( slot_str, "main" ) ||
              util::str_compare_ci( slot_str, "main_hand" ) )
    {
      swap_type = SWAP_MAIN_HAND;
    }
    else if ( util::str_compare_ci( slot_str, "off" ) ||
              util::str_compare_ci( slot_str, "off_hand" ) )
    {
      swap_type = SWAP_OFF_HAND;
    }
    else if ( util::str_compare_ci( slot_str, "both" ) )
    {
      swap_type = SWAP_BOTH;
    }

    if ( util::str_compare_ci( swap_to_str, "primary" ) )
    {
      swap_to_type = WEAPON_PRIMARY;
    }
    else if ( util::str_compare_ci( swap_to_str, "secondary" ) )
    {
      swap_to_type = WEAPON_SECONDARY;
    }

    if ( swap_type != SWAP_BOTH )
    {
      if ( ! rogue -> weapon_data[ swap_type ].item_data[ swap_to_type ] )
      {
        background = true;
        sim->errorf( "Player %s weapon_swap: No weapon info for %s/%s",
                     player->name(), slot_str.c_str(), swap_to_str.c_str() );
      }
    }
    else
    {
      if ( ! rogue -> weapon_data[ WEAPON_MAIN_HAND ].item_data[ swap_to_type ] ||
           ! rogue -> weapon_data[ WEAPON_OFF_HAND ].item_data[ swap_to_type ] )
      {
        background = true;
        sim->errorf( "Player %s weapon_swap: No weapon info for %s/%s",
                     player->name(), slot_str.c_str(), swap_to_str.c_str() );
      }
    }
  }

  result_e calculate_result( action_state_t* ) const override
  { return RESULT_HIT; }

  block_result_e calculate_block_result( action_state_t* ) const override
  { return BLOCK_RESULT_UNBLOCKED; }

  void execute() override
  {
    action_t::execute();

    if ( swap_type == SWAP_MAIN_HAND )
    {
      rogue -> swap_weapon( WEAPON_MAIN_HAND, swap_to_type );
    }
    else if ( swap_type == SWAP_OFF_HAND )
    {
      rogue -> swap_weapon( WEAPON_OFF_HAND, swap_to_type );
    }
    else if ( swap_type == SWAP_BOTH )
    {
      rogue -> swap_weapon( WEAPON_MAIN_HAND, swap_to_type );
      rogue -> swap_weapon( WEAPON_OFF_HAND, swap_to_type );
    }
  }

  bool ready() override
  {
    if ( swap_type == SWAP_MAIN_HAND &&
         rogue -> weapon_data[ WEAPON_MAIN_HAND ].current_weapon == swap_to_type )
    {
      return false;
    }
    else if ( swap_type == SWAP_OFF_HAND &&
              rogue -> weapon_data[ WEAPON_OFF_HAND ].current_weapon == swap_to_type )
    {
      return false;
    }
    else if ( swap_type == SWAP_BOTH &&
              rogue -> weapon_data[ WEAPON_MAIN_HAND ].current_weapon == swap_to_type &&
              rogue -> weapon_data[ WEAPON_OFF_HAND ].current_weapon == swap_to_type )
    {
      return false;
    }

    return action_t::ready();
  }

};

} // end namespace actions

// ==========================================================================
// Expressions
// ==========================================================================

// rogue_attack_t::create_expression =========================================

template<typename Base>
std::unique_ptr<expr_t> actions::rogue_action_t<Base>::create_expression( util::string_view name_str )
{
  if ( util::str_compare_ci( name_str, "cp_gain" ) )
  {
    return make_mem_fn_expr( "cp_gain", *this, &base_t::generate_cp );
  }
  // Garrote and Rupture and APL lines using "exsanguinated"
  // TODO: Add Internal Bleeding (not the same id as Kidney Shot)
  else if ( util::str_compare_ci( name_str, "exsanguinated" ) && 
    ( ab::data().id() == 703 || ab::data().id() == 1943 ||
      this->name_str == "crimson_tempest" || this->name_str == "serrated_bone_spike" ) )
  {
    return make_fn_expr( name_str, [ this ]() {
      dot_t* d = ab::get_dot( ab::target );
      return d->is_ticking() && cast_state( d->state )->is_exsanguinated();
    } );
  }
  else if ( util::str_compare_ci( name_str, "exsanguinated_rate" ) &&
    ( ab::data().id() == 703 || ab::data().id() == 1943 ||
      this->name_str == "crimson_tempest" || this->name_str == "serrated_bone_spike" ) )
  {
    return make_fn_expr( name_str, [ this ]() {
      dot_t* d = ab::get_dot( ab::target );
      return d->is_ticking() ? cast_state( d->state )->get_exsanguinated_rate() : 1.0;
    } );
  }
  else if ( util::str_compare_ci( name_str, "will_lose_exsanguinate" ) &&
    ( ab::data().id() == 703 || ab::data().id() == 1943
      || this->name_str == "crimson_tempest" || this->name_str == "serrated_bone_spike" ) )
  {
    return make_fn_expr( name_str, [ this ]() {
      dot_t* d = ab::get_dot( ab::target );
      double refresh_rate = 1.0;
      return d->is_ticking() && cast_state( d->state )->get_exsanguinated_rate() > refresh_rate;
    } );
  }
  else if ( name_str == "effective_combo_points" )
  {
    return make_fn_expr( name_str, [ this ]() { return p()->current_effective_cp( consumes_echoing_reprimand(), true ); } );
  }

  return ab::create_expression( name_str );
}

// ==========================================================================
// Secondary weapon handling
// ==========================================================================

weapon_slot_e weapon_info_t::slot() const
{
  if ( item_data[ WEAPON_PRIMARY ] -> slot == SLOT_MAIN_HAND )
  {
    return WEAPON_MAIN_HAND;
  }
  else
  {
    return WEAPON_OFF_HAND;
  }
}

void weapon_info_t::callback_state( current_weapon_e weapon, bool state )
{
  sim_t* sim = item_data[ WEAPON_PRIMARY ]->sim;
  for ( size_t i = 0, end = cb_data[ weapon ].size(); i < end; ++i )
  {
    if ( state )
    {
      cb_data[ weapon ][ i ]->activate();
      if ( cb_data[ weapon ][ i ]->rppm )
      {
        cb_data[ weapon ][ i ]->rppm->set_accumulated_blp( timespan_t::zero() );
        cb_data[ weapon ][ i ]->rppm->set_last_trigger_attempt( sim->current_time() );
      }
      sim->print_debug( "{} enabling callback {} on {}", *item_data[ WEAPON_PRIMARY ]->player,
                        cb_data[ weapon ][ i ]->effect, *item_data[ weapon ] );
    }
    else
    {
      cb_data[ weapon ][ i ] -> deactivate();
      sim->print_debug( "{} disabling callback {} on {}", *item_data[ WEAPON_PRIMARY ]->player,
                        cb_data[ weapon ][ i ]->effect, *item_data[ weapon ] );
    }
  }
}

void weapon_info_t::initialize()
{
  if ( initialized )
  {
    return;
  }

  rogue_t* rogue = debug_cast<rogue_t*>( item_data[ WEAPON_PRIMARY ] -> player );

  // Compute stats and initialize the callback data for the weapon. This needs to be done
  // reasonably late (currently in weapon_swap_t action init) to ensure that everything has been
  // initialized.
  if ( item_data[ WEAPON_PRIMARY ] )
  {
    // Find primary weapon callbacks from the actor list of all callbacks
    for ( size_t i = 0; i < item_data[ WEAPON_PRIMARY ] -> parsed.special_effects.size(); ++i )
    {
      special_effect_t* effect = item_data[ WEAPON_PRIMARY ] -> parsed.special_effects[ i ];

      for ( auto* callback : rogue->callbacks.all_callbacks )
      {
        dbc_proc_callback_t* cb = debug_cast<dbc_proc_callback_t*>( callback );

        if ( &( cb -> effect ) == effect )
        {
          cb_data[ WEAPON_PRIMARY ].push_back( cb );
        }
      }
    }

    // Pre-compute primary weapon stats
    for ( stat_e i = STAT_NONE; i < STAT_MAX; i++ )
    {
      stats_data[ WEAPON_PRIMARY ].add_stat( rogue -> convert_hybrid_stat( i ),
                                             item_data[ WEAPON_PRIMARY ] -> stats.get_stat( i ) );
    }
  }

  if ( item_data[ WEAPON_SECONDARY ] )
  {
    // Find secondary weapon callbacks from the actor list of all callbacks
    for ( size_t i = 0; i < item_data[ WEAPON_SECONDARY ] -> parsed.special_effects.size(); ++i )
    {
      special_effect_t* effect = item_data[ WEAPON_SECONDARY ] -> parsed.special_effects[ i ];

      for (auto & callback : rogue -> callbacks.all_callbacks)
      {
        dbc_proc_callback_t* cb = debug_cast<dbc_proc_callback_t*>( callback );

        if ( &( cb -> effect ) == effect )
        {
          cb_data[ WEAPON_SECONDARY ].push_back( cb );
        }
      }
    }

    // Pre-compute secondary weapon stats
    for ( stat_e i = STAT_NONE; i < STAT_MAX; i++ )
    {
      stats_data[ WEAPON_SECONDARY ].add_stat( rogue -> convert_hybrid_stat( i ),
                                               item_data[ WEAPON_SECONDARY ] -> stats.get_stat( i ) );
    }

    if ( item_data[ WEAPON_SECONDARY ] )
    {
      std::string prefix = slot() == WEAPON_MAIN_HAND ? "_mh" : "_oh";

      secondary_weapon_uptime = make_buff( rogue, "secondary_weapon" + prefix );
    }
  }

  initialized = true;
}

void weapon_info_t::reset()
{
  rogue_t* rogue = debug_cast<rogue_t*>( item_data[ WEAPON_PRIMARY ] -> player );

  // Reset swaps back to primary weapon for the slot
  rogue -> swap_weapon( slot(), WEAPON_PRIMARY, false );

  // .. and always deactivates secondary weapon callback(s).
  callback_state( WEAPON_SECONDARY, false );
}

namespace buffs {
// ==========================================================================
// Buffs
// ==========================================================================

struct adrenaline_rush_t : public buff_t
{
  adrenaline_rush_t( rogue_t* p ) :
    buff_t( p, "adrenaline_rush", p->talent.outlaw.adrenaline_rush ) // DFALPHA -- Check RO triggering for other specs
  {
    set_cooldown( timespan_t::zero() );
    set_default_value_from_effect_type( A_MOD_RANGED_AND_MELEE_ATTACK_SPEED );
    set_affects_regen( true );
    add_invalidate( CACHE_ATTACK_SPEED );
    if ( p->legendary.celerity.ok() )
      add_invalidate( CACHE_PLAYER_DAMAGE_MULTIPLIER );
  }

  void start( int stacks, double value, timespan_t duration ) override
  {
    buff_t::start( stacks, value, duration );

    rogue_t* rogue = debug_cast<rogue_t*>( source );
    rogue->resources.temporary[ RESOURCE_ENERGY ] += data().effectN( 4 ).base_value();
    rogue->recalculate_resource_max( RESOURCE_ENERGY );

    if ( rogue->talent.outlaw.improved_adrenaline_rush->ok() )
    {
      int cp_gain = as<int>( rogue->spec.improved_adrenaline_rush_energize->effectN( 1 ).resource() );
      rogue->resource_gain( RESOURCE_COMBO_POINT, cp_gain, rogue->gains.improved_adrenaline_rush );
    }
  }

  void expire_override(int expiration_stacks, timespan_t remaining_duration ) override
  {
    buff_t::expire_override( expiration_stacks, remaining_duration );

    rogue_t* rogue = debug_cast<rogue_t*>( source );
    rogue->resources.temporary[ RESOURCE_ENERGY ] -= data().effectN( 4 ).base_value();
    rogue->recalculate_resource_max( RESOURCE_ENERGY, rogue->gains.adrenaline_rush_expiry );

    if ( rogue->talent.outlaw.improved_adrenaline_rush->ok() )
    {
      int cp_gain = as<int>( rogue->spec.improved_adrenaline_rush_energize->effectN( 1 ).resource() );
      rogue->resource_gain( RESOURCE_COMBO_POINT, cp_gain, rogue->gains.improved_adrenaline_rush_expiry );
    }
  }
};

struct blade_flurry_t : public buff_t
{
  blade_flurry_t( rogue_t* p ) :
    buff_t( p, "blade_flurry", p->talent.outlaw.blade_flurry )
  {
    set_cooldown( timespan_t::zero() );
    set_default_value_from_effect( 2 );
    apply_affecting_aura( p->talent.outlaw.dancing_steel );
  }
};

struct subterfuge_t : public buff_t
{
  rogue_t* rogue;

  subterfuge_t( rogue_t* r ) :
    buff_t( r, "subterfuge", r->spell.subterfuge_buff ),
    rogue( r )
  { }
};

template <typename BuffBase>
struct stealth_like_buff_t : public BuffBase
{
  using base_t = stealth_like_buff_t<BuffBase>;
  rogue_t* rogue;

  stealth_like_buff_t( rogue_t* r, util::string_view name, const spell_data_t* spell ) :
    BuffBase( r, name, spell ), rogue( r )
  {
  }

  void execute( int stacks, double value, timespan_t duration ) override
  {
    bb::execute( stacks, value, duration );

    if ( rogue->stealthed( STEALTH_BASIC ) )
    {
      if ( rogue->legendary.master_assassins_mark->ok() )
        rogue->buffs.master_assassins_mark_aura->trigger();
    }

    if ( rogue->stealthed( STEALTH_BASIC | STEALTH_SHADOWDANCE ) )
    {
      if ( rogue->talent.assassination.master_assassin->ok() )
        rogue->buffs.master_assassin_aura->trigger();

      if ( rogue->talent.assassination.improved_garrote->ok() )
        rogue->buffs.improved_garrote_aura->trigger();

      if ( rogue->talent.outlaw.take_em_by_surprise->ok() )
        rogue->buffs.take_em_by_surprise_aura->trigger();

      if ( rogue->talent.subtlety.premeditation->ok() )
        rogue->buffs.premeditation->trigger();

      if ( rogue->talent.subtlety.shot_in_the_dark->ok() )
        rogue->buffs.shot_in_the_dark->trigger();

      if ( rogue->talent.subtlety.silent_storm->ok() )
        rogue->buffs.silent_storm->trigger();
    }
  }

  void expire_override( int expiration_stacks, timespan_t remaining_duration ) override
  {
    bb::expire_override( expiration_stacks, remaining_duration );

    // Don't swap these buffs around if we are still in stealth due to Vanish expiring
    if ( !rogue->stealthed( STEALTH_BASIC ) )
    {
      rogue->buffs.improved_garrote_aura->expire();
      rogue->buffs.master_assassin_aura->expire();
      rogue->buffs.master_assassins_mark_aura->expire();
      rogue->buffs.take_em_by_surprise_aura->expire();
    }
  }

private:
  using bb = BuffBase;
};

// Note, stealth buff is set a max time of half the nominal fight duration, so it can be
// forced to show in sample sequence tables.
struct stealth_t : public stealth_like_buff_t<buff_t>
{
  stealth_t( rogue_t* r ) :
    base_t( r, "stealth", r->spell.stealth )
  {
    set_duration( sim->max_time / 2 );
  }

  void execute( int stacks, double value, timespan_t duration ) override
  {
    base_t::execute( stacks, value, duration );
    rogue->cancel_auto_attacks();

    // Activating stealth buff (via expiring Vanish) also removes Shadow Dance. Not an issue in general since Stealth cannot be used during Dance.
    rogue->buffs.shadow_dance->expire();
  }
};

// Vanish now acts like "stealth like abilities".
struct vanish_t : public stealth_like_buff_t<buff_t>
{
  std::vector<cooldown_t*> shadowdust_cooldowns;
  const timespan_t shadowdust_reduction;

  vanish_t( rogue_t* r ) :
    base_t( r, "vanish", r->spell.vanish_buff ),
    shadowdust_reduction( timespan_t::from_seconds( r->spec.invigorating_shadowdust_cdr->effectN( 1 ).base_value() ) )
  {
    // TOCHECK DFALPHA -- Make sure this list is updated for Sub
    if ( r->talent.subtlety.invigorating_shadowdust || r->legendary.invigorating_shadowdust.ok() || r->options.prepull_shadowdust )
    {
      shadowdust_cooldowns = { r->cooldowns.adrenaline_rush, r->cooldowns.between_the_eyes, r->cooldowns.blade_flurry,
        r->cooldowns.blade_rush, r->cooldowns.blind, r->cooldowns.cloak_of_shadows, r->cooldowns.deathmark,
        r->cooldowns.dreadblades, r->cooldowns.echoing_reprimand, r->cooldowns.flagellation, r->cooldowns.fleshcraft,
        r->cooldowns.garrote, r->cooldowns.ghostly_strike, r->cooldowns.gouge, r->cooldowns.grappling_hook,
        r->cooldowns.indiscriminate_carnage, r->cooldowns.keep_it_rolling, r->cooldowns.killing_spree, r->cooldowns.kingsbane,
        r->cooldowns.marked_for_death, r->cooldowns.riposte, r->cooldowns.roll_the_bones, r->cooldowns.secret_technique,
        r->cooldowns.sepsis, r->cooldowns.serrated_bone_spike, r->cooldowns.shadow_blades, r->cooldowns.shadow_dance,
        r->cooldowns.shiv, r->cooldowns.sprint, r->cooldowns.symbols_of_death, r->cooldowns.thistle_tea };
    }
  }

  void execute( int stacks, double value, timespan_t duration ) override
  {
    base_t::execute( stacks, value, duration );
    rogue->cancel_auto_attacks();

    // Confirmed on early beta that Invigorating Shadowdust triggers from Vanish buff (via old Sepsis), not just Vanish casts
    if ( rogue->talent.subtlety.invigorating_shadowdust || rogue->legendary.invigorating_shadowdust.ok() ||
         ( rogue->options.prepull_shadowdust && rogue->sim->current_time() == 0_s ) )
    {
      for ( cooldown_t* c : shadowdust_cooldowns )
      {
        if ( c && c->down() )
          c->adjust( -shadowdust_reduction, false );
      }
    }

    // Confirmed on early beta that Deathly Shadows triggers from Vanish buff (via old Sepsis), not just Vanish casts
    if ( rogue->buffs.deathly_shadows->trigger() )
    {
      const int combo_points = as<int>( rogue->buffs.deathly_shadows->data().effectN( 4 ).base_value() );
      rogue->resource_gain( RESOURCE_COMBO_POINT, combo_points, rogue->gains.deathly_shadows );
    }
  }

  void expire_override( int expiration_stacks, timespan_t remaining_duration ) override
  {
    // Stealth proc if Vanish fully ends (i.e. isn't broken before the expiration)
    if ( remaining_duration == timespan_t::zero() )
    {
      rogue->buffs.stealth->trigger();
    }

    base_t::expire_override( expiration_stacks, remaining_duration );
  }
};

// Shadow dance acts like "stealth like abilities"
struct shadow_dance_t : public stealth_like_buff_t<damage_buff_t>
{
  shadow_dance_t( rogue_t* p ) :
    base_t( p, "shadow_dance", p->spell.shadow_dance )
  {
    apply_affecting_aura( p->talent.subtlety.dark_shadow );
    apply_affecting_aura( p->talent.subtlety.improved_shadow_dance );
  }

  void execute( int stacks, double value, timespan_t duration ) override
  {
    stealth_like_buff_t::execute( stacks, value, duration );

    if ( rogue->talent.subtlety.danse_macabre->ok() )
    {
      rogue->buffs.danse_macabre->expire();
      rogue->danse_macabre_tracker.clear();
    }
  }

  void expire_override( int expiration_stacks, timespan_t remaining_duration ) override
  {
    stealth_like_buff_t::expire_override( expiration_stacks, remaining_duration );

    if ( rogue->talent.subtlety.lingering_shadow->ok() )
    {
      rogue->buffs.lingering_shadow->cancel();
      rogue->buffs.lingering_shadow->trigger( rogue->buffs.lingering_shadow->max_stack() );
    }

    if ( rogue->talent.subtlety.danse_macabre->ok() )
    {
      rogue->buffs.danse_macabre->expire();
      rogue->danse_macabre_tracker.clear();
    }

    // These buffs do not persist after Shadow Dance expires, unlike normal Stealth
    rogue->buffs.improved_garrote->expire();
    rogue->buffs.master_assassin->expire();
  }
};

struct shuriken_tornado_t : public buff_t
{
  rogue_t* rogue;
  actions::shuriken_storm_t* shuriken_storm_action;

  shuriken_tornado_t( rogue_t* r ) :
    buff_t( r, "shuriken_tornado", r->talent.subtlety.shuriken_tornado ),
    rogue( r ),
    shuriken_storm_action( nullptr )
  {
    set_cooldown( timespan_t::zero() );
    set_period( timespan_t::from_seconds( 1.0 ) ); // Not explicitly in spell data

    shuriken_storm_action = r->get_secondary_trigger_action<actions::shuriken_storm_t>(
      secondary_trigger::SHURIKEN_TORNADO, "shuriken_storm_tornado" );
    shuriken_storm_action->callbacks = false; // 2021-07-19-- Damage triggered directly, doesn't appear to proc anything
    set_tick_callback( [ this ]( buff_t*, int, timespan_t ) {
      shuriken_storm_action->trigger_secondary_action( rogue->target );
    } );
  }
};

struct slice_and_dice_t : public buff_t
{
  struct recuperator_t : public actions::rogue_heal_t
  {
    recuperator_t( util::string_view name, rogue_t* p ) :
      rogue_heal_t( name, p, p->spell.slice_and_dice )
    {
      // Treat this as direct to avoid duration issues
      direct_tick = true;
      may_crit = false;
      dot_duration = timespan_t::zero();
      base_pct_heal = ( p->talent.rogue.recuperator->ok() ?
                        p->talent.rogue.recuperator->effectN( 1 ).percent() :
                        p->conduit.recuperator.percent() );
      base_dd_min = base_dd_max = 1; // HAX: Make it always heal at least one even without conduit, to allow procing herbs.
      base_costs.fill( 0 );
    }
  };

  rogue_t* rogue;
  recuperator_t* recuperator;

  slice_and_dice_t( rogue_t* p ) :
    buff_t( p, "slice_and_dice", p->spell.slice_and_dice ),
    rogue( p ),
    recuperator( nullptr )
  {
    set_period( data().effectN( 2 ).period() ); // Not explicitly in spell data
    set_default_value_from_effect_type( A_MOD_RANGED_AND_MELEE_ATTACK_SPEED );
    set_refresh_behavior( buff_refresh_behavior::PANDEMIC );
    add_invalidate( CACHE_ATTACK_SPEED );

    // 2020-11-14- Recuperator triggers can proc periodic healing triggers even when 0 value
    if ( p->conduit.recuperator.ok() || p->talent.rogue.recuperator->ok() || p->bugs )
    {
      recuperator = p->get_background_action<recuperator_t>( "recuperator" );
    }

    set_tick_callback( [ this ]( buff_t*, int, timespan_t ) {
      if ( rogue->legendary.celerity.ok() && rogue->specialization() == ROGUE_OUTLAW )
      {
        if ( rng().roll( rogue->legendary.celerity->effectN( 2 ).percent() ) )
        {
          timespan_t duration = timespan_t::from_seconds( rogue->legendary.celerity->effectN( 3 ).base_value() );
          rogue->buffs.adrenaline_rush->extend_duration_or_trigger( duration );
        }
      }

      if ( recuperator )
      {
        recuperator->set_target( rogue );
        recuperator->execute();
      }
    } );
  }
};

struct rogue_poison_buff_t : public buff_t
{
  rogue_poison_buff_t( rogue_td_t& r, util::string_view name, const spell_data_t* spell ) :
    buff_t( r, name, spell )
  { }
};

struct wound_poison_t : public rogue_poison_buff_t
{
  wound_poison_t( rogue_td_t& r ) :
    rogue_poison_buff_t( r, "wound_poison", debug_cast<rogue_t*>( r.source )->spell.wound_poison->effectN( 1 ).trigger() )
  {
    apply_affecting_aura( debug_cast<rogue_t*>( r.source )->talent.rogue.improved_wound_poison );
  }
};

struct atrophic_poison_t : public rogue_poison_buff_t
{
  atrophic_poison_t( rogue_td_t& r ) :
    rogue_poison_buff_t( r, "atrophic_poison", debug_cast<rogue_t*>( r.source )->talent.rogue.atrophic_poison->effectN( 1 ).trigger() )
  {
  }
};

struct crippling_poison_t : public rogue_poison_buff_t
{
  crippling_poison_t( rogue_td_t& r ) :
    rogue_poison_buff_t( r, "crippling_poison", debug_cast<rogue_t*>( r.source )->spell.crippling_poison->effectN( 1 ).trigger() )
  { }
};

struct numbing_poison_t : public rogue_poison_buff_t
{
  numbing_poison_t( rogue_td_t& r ) :
    rogue_poison_buff_t( r, "numbing_poison", debug_cast<rogue_t*>( r.source )->talent.rogue.numbing_poison->effectN( 1 ).trigger() )
  { }
};

struct roll_the_bones_t : public buff_t
{
  rogue_t* rogue;
  std::array<buff_t*, 6> buffs;
  std::array<proc_t*, 6> procs;

  struct overflow_state
  {
    buff_t* buff;
    timespan_t remains;
  };
  std::vector<overflow_state> overflow_states;

  roll_the_bones_t( rogue_t* r ) :
    buff_t( r, "roll_the_bones", r->talent.outlaw.roll_the_bones ),
    rogue( r )
  {
    set_cooldown( timespan_t::zero() );
    set_period( timespan_t::zero() ); // Disable ticking
    set_refresh_behavior( buff_refresh_behavior::PANDEMIC );

    buffs = {
      rogue->buffs.broadside,
      rogue->buffs.buried_treasure,
      rogue->buffs.grand_melee,
      rogue->buffs.ruthless_precision,
      rogue->buffs.skull_and_crossbones,
      rogue->buffs.true_bearing
    };
  }

  void set_procs()
  {
    procs = {
      rogue->procs.roll_the_bones_1,
      rogue->procs.roll_the_bones_2,
      rogue->procs.roll_the_bones_3,
      rogue->procs.roll_the_bones_4,
      rogue->procs.roll_the_bones_5,
      rogue->procs.roll_the_bones_6
    };
  }

  void extend_secondary_buffs( timespan_t duration )
  {
    for ( auto buff : buffs )
    {
      buff->extend_duration( rogue, duration );
    }

    // TOCHECK -- Extend the current container buff to match
    this->extend_duration( rogue, duration );
  }

  void expire_secondary_buffs()
  {
    for ( auto buff : buffs )
    {
      buff->expire();
    }
  }

  void count_the_odds_trigger( timespan_t duration )
  {
    if ( !rogue->talent.outlaw.count_the_odds->ok() && !rogue->conduit.count_the_odds.ok() )
      return;

    std::vector<buff_t*> inactive_buffs;
    for ( buff_t* buff : buffs )
    {
      if ( !buff->check() )
        inactive_buffs.push_back( buff );
    }

    if ( inactive_buffs.empty() )
    {
      rogue->procs.count_the_odds_capped->occur();
      return;
    }

    unsigned buff_idx = static_cast<int>( rng().range( 0, as<double>( inactive_buffs.size() ) ) );
    inactive_buffs[ buff_idx ]->trigger( duration );
  }

  void overflow_expire( bool save_remains )
  {
    overflow_states.clear();

    if ( !save_remains )
      return;

    for ( buff_t* buff : buffs )
    {
      if ( buff->check() )
      {
        // 2022-06-19 -- 9.2.5: Recent testing shows only buffs with longer duration than existing RtB buffs are preserved
        if ( buff->remains() < remains() )
        {
          rogue->procs.roll_the_bones_wasted->occur();
        }
        else if ( buff->remains() > remains() )
        {
          overflow_states.push_back( { buff, buff->remains() } );
        }
      }
    }
  }

  void overflow_restore()
  {
    if ( overflow_states.empty() )
      return;

    // 2021-03-08 -- 9.0.5: If the same roll as an existing partial buff is in the result, the partial buff is lost
    // 2022-06-09 -- TOCHECK: This may be the cause of odd reroll behavior, need to investgate
    for ( auto &state : overflow_states )
    {
      if ( !state.buff->check() )
      {
        state.buff->trigger( state.remains );
      }
    }
  }

  std::vector<buff_t*> random_roll( bool loaded_dice )
  {
    std::vector<buff_t*> rolled;

    if ( rogue->options.fixed_rtb_odds.empty() )
    {
      // RtB uses hardcoded probabilities since 7.2.5
      // As of 2017-05-18 assume these:
      // -- The current proposal is to reduce the number of dice being rolled from 6 to 5
      // -- and to hand-set probabilities to something like 79% chance for 1-buff, 20% chance
      // -- for 2-buffs, and 1% chance for 5-buffs (yahtzee), bringing the expected value of
      // -- a roll down to 1.24 buffs (plus additional value for synergies between buffs).
      // Source: https://us.battle.net/forums/en/wow/topic/20753815486?page=2#post-21
      // Odds double checked on 2020-03-09.
      rogue->options.fixed_rtb_odds = { 79.0, 20.0, 0.0, 0.0, 1.0, 0.0 };

      if ( rogue->talent.outlaw.sleight_of_hand->ok() )
      {
        rogue->options.fixed_rtb_odds[ 0 ] -= rogue->talent.outlaw.sleight_of_hand->effectN( 1 ).base_value();
        rogue->options.fixed_rtb_odds[ 1 ] += rogue->talent.outlaw.sleight_of_hand->effectN( 1 ).base_value();
      }
      else if ( rogue->conduit.sleight_of_hand->ok() )
      {
        rogue->options.fixed_rtb_odds[ 0 ] -= rogue->conduit.sleight_of_hand.value();
        rogue->options.fixed_rtb_odds[ 1 ] += rogue->conduit.sleight_of_hand.value();
      }
    }

    if ( !rogue->options.fixed_rtb_odds.empty() )
    {
      std::vector<double> current_odds = rogue->options.fixed_rtb_odds;
      if ( loaded_dice )
      {
        // Loaded Dice converts 1 buff chance directly into two buff chance. (2020-03-09)
        current_odds[ 1 ] += current_odds[ 0 ];
        current_odds[ 0 ] = 0.0;
      }

      double odd_sum = 0.0;
      for ( const double& chance : current_odds )
        odd_sum += chance;

      double roll = rng().range( 0.0, odd_sum );
      size_t num_buffs = 0;
      double aggregate = 0.0;
      for ( const double& chance : current_odds )
      {
        aggregate += chance;
        num_buffs++;
        if ( roll < aggregate )
        {
          break;
        }
      }

      std::vector<unsigned> pool = { 0, 1, 2, 3, 4, 5 };
      for ( size_t i = 0; i < num_buffs; i++ )
      {
        unsigned buff = (unsigned)rng().range( 0, (double)pool.size() );
        auto buff_idx = pool[ buff ];
        rolled.push_back( buffs[ buff_idx ] );
        pool.erase( pool.begin() + buff );
      }
    }

    return rolled;
  }

  std::vector<buff_t*> fixed_roll()
  {
    std::vector<buff_t*> rolled;
    range::for_each( rogue->options.fixed_rtb, [ this, &rolled ]( size_t idx )
      { rolled.push_back( buffs[ idx ] ); } );
    return rolled;
  }

  unsigned roll_the_bones( timespan_t duration )
  {
    std::vector<buff_t*> rolled;
    if ( rogue->options.fixed_rtb.empty() )
    {
      rolled = random_roll( rogue->buffs.loaded_dice->up() );
    }
    else
    {
      rolled = fixed_roll();
    }

    for ( auto buff : rolled )
    {
      buff->trigger( duration );
    }

    return as<unsigned>( rolled.size() );
  }

  void execute( int stacks, double value, timespan_t duration ) override
  {
    // 2020-11-21 -- Count the Odds buffs are kept if rerolling in some situations
    overflow_expire( true );

    buff_t::execute( stacks, value, duration );

    expire_secondary_buffs();

    const timespan_t roll_duration = remains();
    const unsigned buffs_rolled = roll_the_bones( roll_duration );

    procs[ buffs_rolled - 1 ]->occur();
    rogue->buffs.loaded_dice->expire();

    overflow_restore();
  }
};

} // end namespace buffs

// ==========================================================================
// Rogue Triggers
// ==========================================================================

void rogue_t::trigger_venomous_wounds_death( player_t* target )
{
  // Don't pollute results at the end-of-iteration deaths of everyone
  if ( sim->event_mgr.canceled )
  {
    return;
  }

  if ( !talent.assassination.venomous_wounds->ok() )
    return;

  rogue_td_t* td = get_target_data( target );
  if ( !td->is_poisoned() )
    return;

  // No end event means it naturally expired
  if ( !td->dots.rupture->is_ticking() || td->dots.rupture->remains() == timespan_t::zero() )
  {
    return;
  }

  // TODO: Exact formula?
  unsigned full_ticks_remaining =
      (unsigned)( td->dots.rupture->remains() / td->dots.rupture->current_action->base_tick_time );
  int replenish = as<int>( talent.assassination.venomous_wounds->effectN( 2 ).base_value() );

  sim->print_debug( "{} venomous_wounds replenish on death: full_ticks={}, ticks_left={}, vw_replenish={}, remaining_time={}",
                    *this, full_ticks_remaining, td->dots.rupture->ticks_left(), replenish, td->dots.rupture->remains() );

  resource_gain( RESOURCE_ENERGY, full_ticks_remaining * replenish, gains.venomous_wounds_death,
                 td->dots.rupture->current_action );
}

void rogue_t::trigger_toxic_onslaught( player_t* target )
{
  if ( !legendary.toxic_onslaught->ok() )
    return;

  // As of 9.1.5 we proc the two major cooldowns that are not part of our own spec.
  // 2022-02-01 -- Vendetta appears to be slightly delayed and applied after the final impact
  //               Shadow Blades and Adrenaline Rush are applied immediately
  const timespan_t trigger_duration = legendary.toxic_onslaught->effectN( 1 ).time_value();

  if ( specialization() == ROGUE_ASSASSINATION )
  {
    buffs.adrenaline_rush->extend_duration_or_trigger( trigger_duration );
    buffs.shadow_blades->extend_duration_or_trigger( trigger_duration );
  }
  else if ( specialization() == ROGUE_OUTLAW )
  {
    /* DFALPHA Deathmark?
    make_event( *sim, [this, target, trigger_duration] {
       } ); */
    buffs.shadow_blades->extend_duration_or_trigger( trigger_duration );
  }
  else if ( specialization() == ROGUE_SUBTLETY )
  {
    /* DFALPHA Deathmark?
    make_event( *sim, [this, target, trigger_duration] {
       } ); */
    buffs.adrenaline_rush->extend_duration_or_trigger( trigger_duration );
  }
}

void rogue_t::do_exsanguinate( dot_t* dot, double rate )
{
  if ( !dot->is_ticking() )
    return;

  auto rs = actions::rogue_attack_t::cast_state( dot->state );
  const double new_rate = rs->get_exsanguinated_rate() * rate;
  const double coeff = 1.0 / rate;

  sim->print_log( "{} exsanguinates {} tick rate by {:.1f} from {:.1f} to {:.1f}",
                  *this, *dot, rate, rs->get_exsanguinated_rate(), new_rate );

  // Since the advent of hasted bleed exsanguinate works differently though.
  // Note: PTR testing shows Exsanguinated compound multiplicatively (2x -> 4x -> 8x -> etc.)
  rs->set_exsanguinated_rate( new_rate );
  dot->adjust_full_ticks( coeff );
}

void rogue_t::trigger_exsanguinate( player_t* target )
{
  if ( !talent.assassination.exsanguinate->ok() )
    return;

  rogue_td_t* td = get_target_data( target );

  double rate = 1.0 + talent.assassination.exsanguinate->effectN( 1 ).percent();
  do_exsanguinate( td->dots.garrote, rate );
  do_exsanguinate( td->dots.internal_bleeding, rate );
  do_exsanguinate( td->dots.rupture, rate );
  do_exsanguinate( td->dots.crimson_tempest, rate );

  do_exsanguinate( td->dots.deathmark, rate );
  do_exsanguinate( td->dots.rupture_deathmark, rate );
  do_exsanguinate( td->dots.garrote_deathmark, rate );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_auto_attack( const action_state_t* /* state */ )
{
  if ( !p()->main_hand_attack || p()->main_hand_attack->execute_event || !p()->off_hand_attack || p()->off_hand_attack->execute_event )
    return;

  if ( !ab::data().flags( spell_attribute::SX_MELEE_COMBAT_START ) )
    return;

  p()->auto_attack->execute();
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_poisons( const action_state_t* state )
{
  if ( !ab::result_is_hit( state->result ) )
    return;

  auto trigger_lethal_poison = [this, state]( rogue_poison_t* poison ) {
    if ( poison )
    {
      // 2021-06-29 -- For reasons unknown, Deadly Poison has its own proc logic
      bool procs_lethal_poison = p()->specialization() == ROGUE_ASSASSINATION &&
        poison->data().id() == p()->talent.assassination.deadly_poison->id() ?
        procs_deadly_poison() : procs_poison();

      if ( procs_lethal_poison )
        poison->trigger( state );
    }
  };

  trigger_lethal_poison( p()->active.lethal_poison );
  trigger_lethal_poison( p()->active.lethal_poison_dtb );

  if ( procs_poison() )
  {
    if ( p()->active.nonlethal_poison )
      p()->active.nonlethal_poison->trigger( state );

    if ( p()->active.nonlethal_poison_dtb )
      p()->active.nonlethal_poison_dtb->trigger( state );
  }
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_seal_fate( const action_state_t* state )
{
  if ( !p()->talent.rogue.seal_fate->ok() )
    return;

  if ( state->result != RESULT_CRIT )
    return;

  if ( !p()->rng().roll( p()->talent.rogue.seal_fate->effectN( 1 ).percent() ) )
    return;

  trigger_combo_point_gain( as<int>( p()->talent.rogue.seal_fate->effectN( 2 ).trigger()->effectN( 1 ).base_value() ),
                            p()->gains.seal_fate, true );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_main_gauche( const action_state_t* state )
{
  if ( !p()->mastery.main_gauche->ok() )
    return;

  if ( state->result_total <= 0 )
    return;

  if ( !ab::result_is_hit( state->result ) )
    return;

  if ( !procs_main_gauche() )
    return;

  double proc_chance = p()->mastery.main_gauche->proc_chance() +
                       p()->talent.outlaw.improved_main_gauche->effectN( 1 ).percent();

  if ( p()->buffs.blade_flurry->check() )
  {
    proc_chance += ( p()->talent.outlaw.ambidexterity->ok() ?
                     p()->talent.outlaw.ambidexterity->effectN( 1 ).percent() :
                     p()->conduit.ambidexterity.percent() );
  }

  if ( !p()->rng().roll( proc_chance ) )
    return;

  p()->active.main_gauche->trigger_secondary_action( state->target );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_fatal_flourish( const action_state_t* state )
{
  if ( !p()->talent.outlaw.fatal_flourish->ok() || !ab::result_is_hit( state->result ) || !procs_fatal_flourish() )
    return;

  double chance = p()->talent.outlaw.fatal_flourish->effectN( 1 ).percent();
  // BFALPHA TOCHECK: Proc chance is normalized by weapon speed (i.e. penalty for using daggers)
  if ( state->action != p()->active.main_gauche && state->action->weapon )
    chance *= state->action->weapon->swing_time.total_seconds() / 2.6;
  if ( !p()->rng().roll( chance ) )
    return;

  double gain = p()->talent.outlaw.fatal_flourish->effectN( 1 ).trigger()->effectN( 1 ).resource( RESOURCE_ENERGY );

  p()->resource_gain( RESOURCE_ENERGY, gain, p()->gains.fatal_flourish, state->action );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_energy_refund()
{
  double energy_restored = ab::last_resource_cost * 0.80;
  p()->resource_gain( RESOURCE_ENERGY, energy_restored, p()->gains.energy_refund );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_poison_bomb( const action_state_t* state )
{
  if ( !p()->talent.assassination.poison_bomb->ok() || !ab::result_is_hit( state->result ) )
    return;

  // They put 25 as value in spell data and divide it by 10 later, it's due to the int restriction.
  const auto rs = cast_state( state );
  if ( p()->rng().roll( p()->talent.assassination.poison_bomb->effectN( 1 ).percent() / 10 * rs->get_combo_points() ) )
  {
    make_event<ground_aoe_event_t>( *p()->sim, p(), ground_aoe_params_t()
      .target( state->target )
      .pulse_time( p()->spec.poison_bomb_driver->duration() / p()->talent.assassination.poison_bomb->effectN( 2 ).base_value() )
      .duration( p()->spec.poison_bomb_driver->duration() )
      .action( p()->active.poison_bomb ) );
  }
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_venomous_wounds( const action_state_t* state )
{
  if ( !p()->talent.assassination.venomous_wounds->ok() )
    return;

  if ( !p()->get_target_data( state->target )->is_poisoned() )
    return;

  if ( !ab::result_is_hit( state->result ) )
    return;

  double chance = p()->talent.assassination.venomous_wounds->proc_chance();
  if ( !p()->rng().roll( chance ) )
    return;

  p()->resource_gain( RESOURCE_ENERGY, p()->talent.assassination.venomous_wounds->effectN( 2 ).base_value(),
                      p()->gains.venomous_wounds );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_blade_flurry( const action_state_t* state )
{
  if ( !procs_blade_flurry() )
    return;

  if ( state->result_total <= 0 )
    return;

  if ( !p()->buffs.blade_flurry->check() )
    return;

  if ( !ab::result_is_hit( state->result ) )
    return;

  if ( p()->sim->active_enemies == 1 )
    return;

  // Compute Blade Flurry modifier
  double multiplier = 1.0;
  if ( p()->talent.outlaw.killing_spree->ok() &&
       ( ab::data().id() == p()->spec.killing_spree_mh_attack->id() ||
         ab::data().id() == p()->spec.killing_spree_oh_attack->id() ) )
  {
    multiplier = p()->talent.outlaw.killing_spree->effectN( 2 ).percent();
  }
  else
  {
    multiplier = p()->buffs.blade_flurry->check_value();
  }

  if ( p()->talent.outlaw.precise_cuts->ok() )
  {
    // Already ignores the main target due to the target_list() being filtered
    const auto num_targets = p()->active.blade_flurry->targets_in_range_list( p()->active.blade_flurry->target_list() ).size();
    const auto max_targets = p()->active.blade_flurry->aoe;
    if ( num_targets < max_targets )
    {
      multiplier += p()->talent.outlaw.precise_cuts->effectN( 1 ).percent() * ( max_targets - num_targets );
    }
  }

  // DFALPHA TOCHECK
  // Between the Eyes crit damage multiplier does not transfer across correctly due to a Shadowlands-specific bug
  //if ( p()->bugs && ab::data().id() == p()->spec.between_the_eyes->id() && state->result == RESULT_CRIT )
  //{
  //  multiplier *= 0.5;
  //}

  // Target multipliers do not replicate to secondary targets, need to reverse them out
  const double target_da_multiplier = ( 1.0 / state->target_da_multiplier );

  // Note: Unmitigated damage as Blade Flurry target mitigation is handled on each impact
  double damage = state->result_total * multiplier * target_da_multiplier;
  player_t* primary_target = state->target;

  p()->sim->print_debug( "{} flurries {} for {:.2f} damage ({:.2f} * {} * {:.3f})", *p(), *this, damage, state->result_total, multiplier, target_da_multiplier );

  // Trigger as an event so that this happens after the impact for proc/RPPM targeting purposes
  // Can't use schedule_execute() since multiple flurries can trigger at the same time due to Main Gauche
  make_event( *p()->sim, 0_ms, [ this, damage, primary_target ]() {
    p()->active.blade_flurry->base_dd_min = damage;
    p()->active.blade_flurry->base_dd_max = damage;
    p()->active.blade_flurry->set_target( primary_target );
    p()->active.blade_flurry->execute();
  } );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_combo_point_gain( int cp, gain_t* gain, bool requires_reaction )
{
  if ( !requires_reaction && p()->buffs.shadow_techniques->check() )
    p()->buffs.shadow_techniques->cancel();

  p()->resource_gain( RESOURCE_COMBO_POINT, cp, gain, this );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_ruthlessness_cp( const action_state_t* state )
{
  if ( !p()->talent.outlaw.ruthlessness->ok() || !affected_by.ruthlessness )
    return;

  if ( !ab::result_is_hit( state->result ) )
    return;

  int cp = cast_state( state )->get_combo_points();
  if ( cp == 0 )
    return;

  double cp_chance = p()->talent.outlaw.ruthlessness->effectN( 1 ).pp_combo_points() * cp / 100.0;
  int cp_gain      = 0;
  if ( cp_chance > 1 )
  {
    cp_gain += 1;
    cp_chance -= 1;
  }

  if ( p()->rng().roll( cp_chance ) )
  {
    cp_gain += 1;
  }

  if ( cp_gain > 0 )
  {
    // TOCHECK -- Technically triggers spell 139546
    trigger_combo_point_gain( cp_gain, p()->gains.ruthlessness );
  }
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_deepening_shadows( const action_state_t* state )
{
  if ( !p()->talent.subtlety.deepening_shadows->ok() || !affected_by.deepening_shadows )
    return;

  int cp = cast_state( state )->get_combo_points();
  if ( cp == 0 )
    return;

  double cdr = p()->talent.subtlety.deepening_shadows->effectN( 1 ).base_value() / 10;
  timespan_t adjustment = timespan_t::from_seconds( -cdr * cp );
  p()->cooldowns.shadow_dance->adjust( adjustment, cp >= 5 );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_shadow_techniques( const action_state_t* state )
{
  if ( !p()->spec.shadow_techniques->ok() || !ab::result_is_hit( state->result ) )
    return;

  p()->sim->print_debug( "{} trigger_shadow_techniques increment from {} to {}", *p(), p()->shadow_techniques_counter, p()->shadow_techniques_counter + 1 );

  // 2021-04-22-- Initial 9.1.0 testing appears to show the threshold is reduced to 4/3
  const unsigned shadow_techniques_upper = 4;
  const unsigned shadow_techniques_lower = 3;
  if ( ++p()->shadow_techniques_counter >= shadow_techniques_upper || ( p()->shadow_techniques_counter == shadow_techniques_lower && p()->rng().roll( 0.5 ) ) )
  {
    // SimC-side tracking buffs for reaction delay
    if ( p()->current_cp() < p()->resources.max[ RESOURCE_COMBO_POINT ] )
    {
      p()->buffs.shadow_techniques->set_default_value( p()->current_cp() );
      p()->buffs.shadow_techniques->trigger();
    }

    p()->sim->print_debug( "{} trigger_shadow_techniques proc'd at {}, resetting counter to 0", *p(), p()->shadow_techniques_counter );
    p()->shadow_techniques_counter = 0;

    double energy_gain = p()->spec.shadow_techniques_energize->effectN( 2 ).base_value() +
                         p()->talent.subtlety.improved_shadow_techniques->effectN( 1 ).base_value();
    p()->resource_gain( RESOURCE_ENERGY, energy_gain, p()->gains.shadow_techniques, state->action );
    trigger_combo_point_gain( as<int>( p()->spec.shadow_techniques_energize->effectN( 1 ).base_value() ),
                              p()->gains.shadow_techniques, true );

    if ( p()->talent.subtlety.stiletto_staccato->ok() )
    {
      p()->cooldowns.shadow_blades->adjust( -p()->talent.subtlety.stiletto_staccato->effectN( 1 ).time_value(), true );
    }
    else if ( p()->conduit.stiletto_staccato.ok() )
    {
      p()->cooldowns.shadow_blades->adjust( -p()->conduit.stiletto_staccato.time_value( conduit_data_t::time_type::S ), true );
    }
  }
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_weaponmaster( const action_state_t* state, actions::rogue_attack_t* action )
{
  if ( !p()->talent.subtlety.weaponmaster->ok() || !ab::result_is_hit( state->result ) || !action )
    return;

  // 2022-02-24 -- 9.2 now allows this to trigger with a per-target ICD
  cooldown_t* tcd = p()->cooldowns.weaponmaster->get_cooldown( state->target );
  if ( !tcd || tcd->down() )
    return;

  if ( !p()->rng().roll( p()->talent.subtlety.weaponmaster->proc_chance() ) )
    return;

  p()->procs.weaponmaster->occur();
  tcd->start();

  p()->sim->print_log( "{} procs weaponmaster for {}", *p(), *this );

  // Direct damage re-computes on execute
  action->trigger_secondary_action( state->target, cast_state( state )->get_combo_points() );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_opportunity( const action_state_t* state, actions::rogue_attack_t* action )
{
  if ( !p()->talent.outlaw.opportunity->ok() || !ab::result_is_hit( state->result ) || !action )
    return;

  const int stacks = 1 + as<int>( p()->talent.outlaw.fan_the_hammer->effectN( 1 ).base_value() );
  if ( p()->buffs.opportunity->trigger( stacks, buff_t::DEFAULT_VALUE(), p()->extra_attack_proc_chance() ) )
  {
    action->trigger_secondary_action( state->target, 300_ms );
    p()->buffs.concealed_blunderbuss->trigger();
  }
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_elaborate_planning( const action_state_t* /* state */ )
{
  if ( !p()->talent.assassination.elaborate_planning->ok() || !affected_by.elaborate_planning || ab::background )
    return;

  p()->buffs.elaborate_planning->trigger();
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_alacrity( const action_state_t* state )
{
  if ( !p()->talent.rogue.alacrity->ok() || !affected_by.alacrity )
    return;

  // TOCHECK -- Double check stack overflow for 5+ CP works correctly
  // DFALPHA -- This appears very bugged on PTR as the chance seems to be scaling with the Points Per Combo Point value
  double chance = p()->talent.rogue.alacrity->effectN( 2 ).percent() * cast_state( state )->get_combo_points();
  int stacks = 0;
  if ( chance > 1 )
  {
    stacks += 1;
    chance -= 1;
  }

  if ( p()->rng().roll( chance ) )
  {
    stacks += 1;
  }

  if ( stacks > 0 )
  {
    p()->buffs.alacrity->trigger( stacks );
  }
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_restless_blades( const action_state_t* state )
{
  timespan_t v = timespan_t::from_seconds( p()->talent.outlaw.restless_blades->effectN( 1 ).base_value() / 10.0 );
  v += timespan_t::from_seconds( p()->buffs.true_bearing->value() );
  v *= -cast_state( state )->get_combo_points();

  // DFALPHA Tooltip:
  // Affected skills: Adrenaline Rush, Between the Eyes, Blade Flurry, Blade Rush, Dreadblades, Ghostly Strike, Grappling Hook, 
  // Keep it Rolling, Killing Spree, Marked for Death, Roll the Bones, Sepsis, Sprint, and Vanish.

  p()->cooldowns.adrenaline_rush->adjust( v, false );
  p()->cooldowns.between_the_eyes->adjust( v, false );
  p()->cooldowns.blade_flurry->adjust( v, false );
  p()->cooldowns.blade_rush->adjust( v, false );
  p()->cooldowns.dreadblades->adjust( v, false );
  p()->cooldowns.ghostly_strike->adjust( v, false );
  p()->cooldowns.grappling_hook->adjust( v, false );
  p()->cooldowns.keep_it_rolling->adjust( v, false );
  p()->cooldowns.killing_spree->adjust( v, false );
  p()->cooldowns.marked_for_death->adjust( v, false );
  p()->cooldowns.roll_the_bones->adjust( v, false );
  p()->cooldowns.sepsis->adjust( v, false );
  p()->cooldowns.sprint->adjust( v, false );
  p()->cooldowns.vanish->adjust( v, false ); // DFALPHA -- Appears bugged

  if ( p()->talent.outlaw.float_like_a_butterfly->ok() )
  {
    p()->cooldowns.evasion->adjust( v, false );
    p()->cooldowns.feint->adjust( v, false );
  }
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_dreadblades( const action_state_t* state )
{
  if ( !p()->talent.outlaw.dreadblades->ok() || !ab::result_is_hit( state->result ) )
    return;

  if ( ab::energize_type == action_energize::NONE || ab::energize_resource != RESOURCE_COMBO_POINT || ab::energize_amount == 0 )
    return;

  // 2022-02-04 -- Due to not being cast triggers, this appears to not work
  if ( is_secondary_action() )
    return;

  if ( !p()->buffs.dreadblades->up() )
    return;

  trigger_combo_point_gain( as<int>( p()->buffs.dreadblades->check_value() ), p()->gains.dreadblades );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_relentless_strikes( const action_state_t* state )
{
  if ( !p()->talent.subtlety.relentless_strikes->ok() || !affected_by.relentless_strikes )
    return;

  double grant_energy = cast_state( state )->get_combo_points() *
    p()->spec.relentless_strikes_energize->effectN( 1 ).resource( RESOURCE_ENERGY );

  if ( grant_energy > 0 )
  {
    p()->resource_gain( RESOURCE_ENERGY, grant_energy, p()->gains.relentless_strikes, state->action );
  }
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_venom_rush( const action_state_t* state )
{
  if ( !p()->talent.assassination.venom_rush->ok() )
    return;

  if ( !ab::result_is_hit( state->result ) || !p()->get_target_data( state->target )->is_poisoned() )
    return;
  
  p()->resource_gain( RESOURCE_ENERGY, p()->talent.assassination.venom_rush->effectN( 1 ).base_value(), p()->gains.venom_rush );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_blindside( const action_state_t* state )
{
  if ( !p()->talent.assassination.blindside->ok() || !ab::result_is_hit( state->result ) )
    return;

  double chance = p()->talent.assassination.blindside->effectN( 1 ).percent();
  if ( state->target->health_percentage() < p()->talent.assassination.blindside->effectN( 3 ).base_value() )
  {
    chance = p()->talent.assassination.blindside->effectN( 2 ).percent();
  }
  if ( p()->rng().roll( chance ) )
  {
    p()->buffs.blindside->trigger();
  }
}

template <typename Base>
void actions::rogue_action_t<Base>::spend_combo_points( const action_state_t* state )
{
  if ( ab::base_costs[ RESOURCE_COMBO_POINT ] == 0 )
    return;

  if ( !ab::hit_any_target )
    return;

  const auto rs = cast_state( state );
  double max_spend = std::min( p()->current_cp(), p()->consume_cp_max() );
  ab::stats->consume_resource( RESOURCE_COMBO_POINT, max_spend );
  p()->resource_loss( RESOURCE_COMBO_POINT, max_spend );
  p()->buffs.shadow_techniques->cancel(); // Remove tracking mechanism after CP spend

  p()->sim->print_log( "{} consumes {} {} for {} ({})", *p(), max_spend, util::resource_type_string( RESOURCE_COMBO_POINT ),
                                                        *this, p()->current_cp() );

  if ( ab::name_str != "secret_technique" )
  {
    // As of 2018-06-28 on beta, Secret Technique does not reduce its own cooldown. May be a bug or the cdr happening
    // before CD start.
    timespan_t sectec_cdr = timespan_t::from_seconds( p()->talent.subtlety.secret_technique->effectN( 5 ).base_value() );
    sectec_cdr *= -max_spend;
    p()->cooldowns.secret_technique->adjust( sectec_cdr, false );
  }

  // Remove Echoing Reprimand Buffs
  if ( p()->spell.echoing_reprimand->ok() && consumes_echoing_reprimand() )
  {
    int base_cp = rs->get_combo_points( true );
    if ( rs->get_combo_points() > base_cp )
    {
      auto it = range::find_if( p()->buffs.echoing_reprimand, [ base_cp ]( const buff_t* buff ) { return buff->check_value() == base_cp; } );
      assert( it != p()->buffs.echoing_reprimand.end() );
      ( *it )->expire();
      p()->procs.echoing_reprimand[ it - p()->buffs.echoing_reprimand.begin() ]->occur();
      animacharged_cp_proc->occur();
    }
  }
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_shadow_blades_attack( action_state_t* state )
{
  if ( !p()->buffs.shadow_blades->check() || state->result_total <= 0 || !ab::result_is_hit( state->result ) || !procs_shadow_blades_damage() )
    return;

  double amount = state->result_amount * p()->buffs.shadow_blades->check_value();
  // Deeper Daggers, despite Shadow Blades having the disable player multipliers flag, affects Shadow Blades with a manual exclusion for Gloomblade.
  // DFALPHA TOCHECK all shadow damage multipliers for this
  if ( p()->buffs.deeper_daggers->check() && ab::data().id() != p()->talent.subtlety.gloomblade->id() )
    amount *= p()->buffs.deeper_daggers->value_direct();

  p()->active.shadow_blades_attack->base_dd_min = amount;
  p()->active.shadow_blades_attack->base_dd_max = amount;
  p()->active.shadow_blades_attack->set_target( state->target );
  p()->active.shadow_blades_attack->execute();
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_prey_on_the_weak( const action_state_t* state )
{
  if ( !state->target->is_boss() || !ab::result_is_hit(state->result) || !p()->talent.rogue.prey_on_the_weak->ok() )
    return;

  td( state->target )->debuffs.prey_on_the_weak->trigger();
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_find_weakness( const action_state_t* state, timespan_t duration )
{
  if ( !ab::result_is_hit( state->result ) || !p()->talent.rogue.find_weakness->ok() )
    return;

  // If the new duration (e.g. from Backstab crits) is lower than the existing debuff duration, refresh without change.
  if ( duration > timespan_t::zero() && duration < td( state->target )->debuffs.find_weakness->remains() )
    duration = td( state->target )->debuffs.find_weakness->remains();

  td( state->target )->debuffs.find_weakness->trigger( duration );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_grand_melee( const action_state_t* state )
{
  if ( !ab::result_is_hit( state->result ) || !p()->buffs.grand_melee->up() )
    return;

  timespan_t snd_extension = cast_state( state )->get_combo_points() * timespan_t::from_seconds( p()->buffs.grand_melee->check_value() );
  p()->buffs.slice_and_dice->extend_duration_or_trigger( snd_extension );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_master_of_shadows()
{
  // Since Stealth from expiring Vanish cannot trigger this but expire_override already treats vanish as gone, we have to do this manually using this function.
  if ( p()->in_combat && p()->talent.subtlety.master_of_shadows->ok() )
  {
    p()->buffs.master_of_shadows->trigger();
  }
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_akaaris_soul_fragment( const action_state_t* state )
{
  if ( !ab::result_is_hit( state->result ) || !p()->legendary.akaaris_soul_fragment->ok() )
    return;

  // 2022-02-07 -- Does not work on Weaponmaster
  if ( is_secondary_action() )
    return;

  td( state->target )->debuffs.akaaris_soul_fragment->trigger();
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_bloodfang( const action_state_t* state )
{
  if ( !ab::result_is_hit( state->result ) || !p()->legendary.essence_of_bloodfang->ok() || !p()->active.bloodfang )
    return;

  if ( ab::energize_type == action_energize::NONE || ab::energize_resource != RESOURCE_COMBO_POINT )
    return;

  if ( p()->active.bloodfang->internal_cooldown->down() )
    return;

  if ( !p()->rng().roll( p()->legendary.essence_of_bloodfang->proc_chance() ) )
    return;

  p()->active.bloodfang->set_target( state->target );
  p()->active.bloodfang->execute();
  p()->active.bloodfang->internal_cooldown->start();
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_dashing_scoundrel( const action_state_t* state )
{
  if ( !affected_by.dashing_scoundrel )
    return;

  // 2021-02-21-- Use the Crit-modifier whitelist to control this as it currently matches
  if ( state->result != RESULT_CRIT || !p()->buffs.envenom->check() )
    return;

  p()->resource_gain( RESOURCE_ENERGY, p()->spec.dashing_scoundrel_gain, p()->gains.dashing_scoundrel );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_guile_charm( const action_state_t* state )
{
  if ( !ab::result_is_hit( state->result ) || !p()->legendary.guile_charm.ok() )
    return;

  if ( p()->buffs.guile_charm_insight_3->check() )
    return;

  // 2021-04-16-- Logs show this is now 6 SS impacts per insight transition
  bool trigger_next_insight = ( ++p()->legendary.guile_charm_counter >= 6 );

  if ( p()->buffs.guile_charm_insight_1->check() )
  {
    if( trigger_next_insight )
      p()->buffs.guile_charm_insight_2->trigger();
    else
      p()->buffs.guile_charm_insight_1->trigger();
  }
  else if ( p()->buffs.guile_charm_insight_2->check() )
  {
    if ( trigger_next_insight )
      p()->buffs.guile_charm_insight_3->trigger();
    else
      p()->buffs.guile_charm_insight_2->trigger();
  }
  else if( trigger_next_insight )
  {
    p()->buffs.guile_charm_insight_1->trigger();
  }
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_count_the_odds( const action_state_t* state )
{
  if ( !p()->talent.outlaw.count_the_odds->ok() && !p()->conduit.count_the_odds.ok() )
    return;

  if ( !ab::result_is_hit( state->result ) )
    return;

  // Currently it appears all Rogues can trigger this with Ambush
  if ( !p()->bugs && p()->specialization() != ROGUE_OUTLAW )
    return;

  // Confirmed via logs this works with Shadowmeld and Shadow Dance
  double stealth_bonus = 1.0;
  if ( p()->stealthed( STEALTH_BASIC | STEALTH_SHADOWMELD | STEALTH_SHADOWDANCE ) )
  {
    stealth_bonus += ( p()->talent.outlaw.count_the_odds->ok() ?
                       p()->talent.outlaw.count_the_odds->effectN( 3 ).percent() :
                       p()->conduit.count_the_odds->effectN( 3 ).percent() );
  }

  double trigger_chance = 0.0;
  timespan_t trigger_duration = 0_s;
  if ( p()->talent.outlaw.count_the_odds->ok() )
  {
    trigger_chance = p()->talent.outlaw.count_the_odds->effectN( 1 ).percent();
    trigger_duration = timespan_t::from_seconds( p()->talent.outlaw.count_the_odds->effectN( 2 ).base_value() );
  }
  else
  {
    trigger_chance = p()->conduit.count_the_odds.percent();
    trigger_duration = timespan_t::from_seconds( p()->conduit.count_the_odds->effectN( 2 ).base_value() );
  }

  if ( !p()->rng().roll( trigger_chance * stealth_bonus ) )
    return;

  debug_cast<buffs::roll_the_bones_t*>( p()->buffs.roll_the_bones )->count_the_odds_trigger( trigger_duration * stealth_bonus );
  p()->procs.count_the_odds->occur();
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_keep_it_rolling()
{
  if ( !p()->talent.outlaw.keep_it_rolling->ok() )
    return;

  timespan_t extend_duration = timespan_t::from_seconds( p()->talent.outlaw.keep_it_rolling->effectN( 1 ).base_value() );
  debug_cast<buffs::roll_the_bones_t*>( p()->buffs.roll_the_bones )->extend_secondary_buffs( extend_duration );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_flagellation( const action_state_t* state )
{
  if ( !affected_by.flagellation )
    return;

  if ( !p()->covenant.flagellation->ok() && !p()->talent.subtlety.flagellation->ok() )
    return;

  buff_t* debuff = p()->active.flagellation->debuff;
  if ( !debuff || !debuff->up() )
    return;

  const auto rs = cast_state( state );
  const int cp_spend = rs->get_combo_points();
  if ( cp_spend <= 0 )
    return;

  p()->buffs.flagellation->trigger( cp_spend );
  
  // 2022-02-06 -- Testing shows that Outlaw 4pc procs trigger buff stacks but not damage/CDR
  if ( is_secondary_action() )
    return;

  p()->active.flagellation->trigger_secondary_action( debuff->player, cp_spend, 0.75_s );
  
  // DFALPHA TOCHECK -- Currently Obedience does nothing on the talent version
  if ( p()->legendary.obedience->ok() && !p()->talent.subtlety.flagellation->ok() )
  {
    // Obedience currently does not benefit from Echoing Reprimand CP
    const int base_cp_spend = rs->get_combo_points( true );
    const timespan_t obedience_cdr = p()->legendary.obedience->effectN( 1 ).time_value() * base_cp_spend;
    p()->cooldowns.flagellation->adjust( -obedience_cdr );
  }

  for ( int i = 0; i < cp_spend; i++ )
  {
    p()->procs.flagellation_cp_spend->occur();
  }
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_perforated_veins( const action_state_t* state )
{
  if ( !p()->spec.perforated_veins_buff->ok() || !ab::result_is_hit(state->result) )
    return;

  // 2022-02-24 -- 9.2 now allows this to trigger from procs with a per-target ICD
  cooldown_t* tcd = p()->cooldowns.perforated_veins->get_cooldown( state->target );
  if ( !tcd || tcd->down() )
    return;

  tcd->start();
  p()->buffs.perforated_veins->trigger();
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_banshees_blight( const action_state_t* state )
{
  if ( !p()->legendary.banshees_blight || !procs_banshees_blight() )
    return;

  // 2022-08-04 -- Further S4 testing shows BtE 4pc procs do not actually trigger the dagger
  if ( is_secondary_action() )
    return;

  const auto rs = cast_state( state );
  int stack_count = td( rs->target )->debuffs.banshees_blight->check();
  if ( stack_count > 0 && p()->rng().roll( rs->get_combo_points() * p()->legendary.banshees_blight->effectN( 2 ).percent() ) )
  {
    // Only executes on the "primary" target of AoE finishers
    player_t* proc_target = rs->target;
    for ( int i = 0; i < stack_count; ++i )
    {
      make_event( *ab::sim, i * 150_ms, [this, proc_target]
      {
        p()->active.banshees_blight->set_target( proc_target );
        p()->active.banshees_blight->execute();
      } );
    }
  }
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_lingering_shadow( const action_state_t* state )
{
  if ( !p()->talent.subtlety.lingering_shadow->ok() || !ab::result_is_hit( state->result ) )
    return;

  // Testing appears to show this does not work on Weaponmaster attacks
  if ( is_secondary_action() )
    return;

  int stacks = p()->buffs.lingering_shadow->stack();
  if ( stacks < 1 )
    return;

  // Tooltips imply a bonus of 1% per stack, appears to use mitigated result
  double amount = state->result_mitigated * ( stacks / 100.0 );
  p()->active.lingering_shadow->execute_on_target( state->target, amount );
}

template <typename Base>
void actions::rogue_action_t<Base>::trigger_danse_macabre( const action_state_t* state )
{
  if ( !p()->talent.subtlety.danse_macabre->ok() )
    return;

  if ( ab::background || ab::trigger_gcd == 0_ms || !affected_by.danse_macabre )
    return;

  if ( !p()->stealthed( STEALTH_SHADOWDANCE ) )
    return;

  if ( range::contains( p()->danse_macabre_tracker, ab::data().id() ) )
  {
    // Beta has been revamped to not clear the debuff on repeats. Not present in prepatch.
    if ( !p()->is_ptr() )
    {
      p()->danse_macabre_tracker.clear();
      p()->buffs.danse_macabre->expire();
    }
  }
  else
  {
    p()->danse_macabre_tracker.push_back( ab::data().id() );
    p()->buffs.danse_macabre->increment();
  }
}

// ==========================================================================
// Rogue Targetdata Definitions
// ==========================================================================

rogue_td_t::rogue_td_t( player_t* target, rogue_t* source ) :
  actor_target_data_t( target, source ),
  dots( dots_t() ),
  debuffs( debuffs_t() )
{
  dots.crimson_tempest          = target->get_dot( "crimson_tempest", source );
  dots.deadly_poison            = target->get_dot( "deadly_poison_dot", source );
  dots.deadly_poison_deathmark  = target->get_dot( "deadly_poison_dot_deathmark", source );
  dots.deathmark                = target->get_dot( "deathmark", source );
  dots.garrote                  = target->get_dot( "garrote", source );
  dots.garrote_deathmark        = target->get_dot( "garrote_deathmark", source );
  dots.internal_bleeding        = target->get_dot( "internal_bleeding", source );
  dots.kingsbane                = target->get_dot( "kingsbane", source );
  dots.mutilated_flesh          = target->get_dot( "mutilated_flesh", source );
  dots.rupture                  = target->get_dot( "rupture", source );
  dots.rupture_deathmark        = target->get_dot( "rupture_deathmark", source );
  dots.sepsis                   = target->get_dot( "sepsis", source );
  dots.serrated_bone_spike      = target->get_dot( "serrated_bone_spike_dot", source );

  debuffs.wound_poison          = new buffs::wound_poison_t( *this );
  debuffs.atrophic_poison       = new buffs::atrophic_poison_t( *this );
  debuffs.crippling_poison      = new buffs::crippling_poison_t( *this );
  debuffs.numbing_poison        = new buffs::numbing_poison_t( *this );

  debuffs.amplifying_poison = make_buff( *this, "amplifying_poison", source->spec.amplifying_poison_debuff );
  debuffs.amplifying_poison_deathmark = make_buff( *this, "amplifying_poison_deathmark", source->spec.deathmark_amplifying_poison );
  
  debuffs.deathmark = make_buff<damage_buff_t>( *this, "deathmark", source->spec.deathmark_debuff );
  debuffs.deathmark->set_cooldown( timespan_t::zero() );
  
  debuffs.marked_for_death = make_buff( *this, "marked_for_death", source->talent.rogue.marked_for_death )
    ->set_cooldown( timespan_t::zero() );
  debuffs.shiv = make_buff<damage_buff_t>( *this, "shiv", source->spec.improved_shiv_debuff )
    ->apply_affecting_conduit( source->conduit.well_placed_steel );
  debuffs.ghostly_strike = make_buff( *this, "ghostly_strike", source->talent.outlaw.ghostly_strike )
    ->set_default_value_from_effect_type( A_MOD_DAMAGE_FROM_CASTER )
    ->set_cooldown( timespan_t::zero() );
  debuffs.find_weakness = make_buff( *this, "find_weakness", source->spell.find_weakness_debuff )
    ->set_default_value( source->talent.rogue.find_weakness->effectN( 1 ).percent() );
  debuffs.prey_on_the_weak = make_buff( *this, "prey_on_the_weak", source->spell.prey_on_the_weak_debuff )
    ->set_default_value_from_effect_type( A_MOD_DAMAGE_PERCENT_TAKEN );
  debuffs.between_the_eyes = make_buff( *this, "between_the_eyes", source->spec.between_the_eyes )
    ->set_default_value_from_effect_type( A_MOD_CRIT_CHANCE_FROM_CASTER )
    ->set_cooldown( timespan_t::zero() );
  debuffs.flagellation = make_buff( *this, "flagellation", source->spec.flagellation_buff )
    ->set_initial_stack( 1 )
    ->set_refresh_behavior( buff_refresh_behavior::DISABLED )
    ->set_period( timespan_t::zero() )
    ->set_cooldown( timespan_t::zero() );

  if ( source->legendary.akaaris_soul_fragment.ok() )
  {
    debuffs.akaaris_soul_fragment = make_buff( *this, "akaaris_soul_fragment", source->find_spell( 341111 ) )
      ->set_refresh_behavior( buff_refresh_behavior::PANDEMIC )
      ->set_tick_behavior( buff_tick_behavior::REFRESH )
      ->set_tick_callback( [ source, target ]( buff_t*, int, timespan_t ) {
        source->active.akaaris_shadowstrike->trigger_secondary_action( target );
      } )
      ->set_partial_tick( true );
  }

  // Slyvanas Dagger
  if ( source->legendary.banshees_blight )
    debuffs.banshees_blight = make_buff( *this, "banshees_blight", source->find_spell( 358090 ) );
  else
    debuffs.banshees_blight = make_buff( *this, "banshees_blight" )->set_quiet( true );

  // Type-Based Tracking for Accumulators
  bleeds = { dots.deathmark, dots.garrote, dots.garrote_deathmark, dots.internal_bleeding,
             dots.rupture, dots.rupture_deathmark, dots.crimson_tempest, dots.mutilated_flesh,
             dots.serrated_bone_spike };
  poison_dots = { dots.deadly_poison, dots.deadly_poison_deathmark, dots.sepsis, dots.kingsbane };
  poison_debuffs = { debuffs.atrophic_poison, debuffs.crippling_poison, debuffs.numbing_poison,
                     debuffs.wound_poison, debuffs.amplifying_poison, debuffs.amplifying_poison_deathmark };

  // Callbacks ================================================================

  // Marked for Death Reset
  if ( source->talent.rogue.marked_for_death->ok() )
  {
    target->register_on_demise_callback( source, [ this, source ]( player_t* demise_target ) {
      if ( debuffs.marked_for_death->up() && !demise_target->debuffs.invulnerable->check() )
        source->cooldowns.marked_for_death->reset( true );
    } );
  }

  // Venomous Wounds Energy Refund
  if ( source->talent.assassination.venomous_wounds->ok() )
  {
    target->register_on_demise_callback( source, [source](player_t* target) { source->trigger_venomous_wounds_death( target ); } );
  }

  // Sepsis Cooldown Reduction
  if ( source->covenant.sepsis->ok() )
  {
    target->register_on_demise_callback( source, [ this, source ]( player_t* target ) {
      if ( dots.sepsis->is_ticking() )
      {
        source->cooldowns.sepsis->adjust( -timespan_t::from_seconds( source->covenant.sepsis->effectN( 3 ).base_value() ) );
        source->trigger_toxic_onslaught( target );
      }
    } );
  }

  // Serrated Bone Spike Charge Refund
  if ( source->covenant.serrated_bone_spike->ok() || source->talent.assassination.serrated_bone_spike->ok() )
  {
    target->register_on_demise_callback( source, [ this, source ]( player_t* ) {
      if ( dots.serrated_bone_spike->is_ticking() )
      {
        double refund_max = source->cooldowns.serrated_bone_spike->charges - source->cooldowns.serrated_bone_spike->charges_fractional();
        if ( refund_max > 1 )
          source->procs.serrated_bone_spike_refund->occur();
        else if ( refund_max <= 0 )
          source->procs.serrated_bone_spike_waste->occur();
        else
          source->procs.serrated_bone_spike_waste_partial->occur();

        source->cooldowns.serrated_bone_spike->reset( false, 1 );
      }
    } );
  }

  // Flagellation On-Death Buff Trigger
  // DFALPHA TOCHECK -- Confirm the talent version gives bonus death stacks
  if ( source->covenant.flagellation->ok() || source->talent.subtlety.flagellation->ok() )
  {
    target->register_on_demise_callback( source, [ this, source ]( player_t* ) {
      if ( debuffs.flagellation->check() )
      {
        source->buffs.flagellation->increment( 10 );
        source->buffs.flagellation->expire(); // Triggers persist buff
      }
    } );
  }
}

// ==========================================================================
// Rogue Character Definition
// ==========================================================================

// rogue_t::composite_attribute_multiplier ==================================

double rogue_t::composite_attribute_multiplier( attribute_e a ) const
{
  double am = player_t::composite_attribute_multiplier( a );

  if ( a == ATTR_STAMINA )
  {
    // TOCHECK -- This may be a base attribute multiplier now
    am *= 1.0 + talent.outlaw.combat_stamina->effectN( 1 ).percent();
  }

  return am;
}

// rogue_t::composite_melee_speed ===========================================

double rogue_t::composite_melee_speed() const
{
  double h = player_t::composite_melee_speed();

  if ( buffs.slice_and_dice->check() )
  {
    h *= 1.0 / ( 1.0 + buffs.slice_and_dice->check_value() );
  }

  if ( buffs.adrenaline_rush->check() )
  {
    h *= 1.0 / ( 1.0 + buffs.adrenaline_rush->check_value() );
  }

  return h;
}

// rogue_t::composite_melee_haste ==========================================

double rogue_t::composite_melee_haste() const
{
  double h = player_t::composite_melee_haste();

  if ( buffs.alacrity->check() )
  {
    h *= 1.0 / ( 1.0 + buffs.alacrity->check_stack_value() );
  }

  // Talent version gives Mastery, not Haste
  if ( !talent.subtlety.flagellation->ok() )
  {
    if ( buffs.flagellation->check() )
    {
      h *= 1.0 / ( 1.0 + buffs.flagellation->check_stack_value() );
    }
    if ( buffs.flagellation_persist->check() )
    {
      h *= 1.0 / ( 1.0 + buffs.flagellation_persist->check_stack_value() );
    }
  }

  return h;
}

// rogue_t::composite_melee_crit_chance =========================================

double rogue_t::composite_melee_crit_chance() const
{
  double crit = player_t::composite_melee_crit_chance();

  crit += spell.critical_strikes->effectN( 1 ).percent();
  crit += talent.rogue.lethality->effectN( 1 ).percent();

  if ( conduit.planned_execution.ok() && buffs.symbols_of_death->up() )
  {
    crit += conduit.planned_execution.percent();
  }

  if ( talent.subtlety.planned_execution->ok() && buffs.symbols_of_death->up() )
  {
    crit += talent.subtlety.planned_execution->effectN( 1 ).percent();
  }

  return crit;
}

// rogue_t::composite_spell_crit_chance =========================================

double rogue_t::composite_spell_crit_chance() const
{
  double crit = player_t::composite_spell_crit_chance();

  crit += spell.critical_strikes->effectN( 1 ).percent();
  crit += talent.rogue.lethality->effectN( 1 ).percent();

  if ( conduit.planned_execution.ok() && buffs.symbols_of_death->up() )
  {
    crit += conduit.planned_execution.percent();
  }

  if ( talent.subtlety.planned_execution->ok() && buffs.symbols_of_death->up() )
  {
    crit += talent.subtlety.planned_execution->effectN( 1 ).percent();
  }

  return crit;
}

// rogue_t::composite_spell_haste ==========================================

double rogue_t::composite_spell_haste() const
{
  double h = player_t::composite_spell_haste();

  if ( buffs.alacrity->check() )
  {
    h *= 1.0 / ( 1.0 + buffs.alacrity->check_stack_value() );
  }

  // Talent version gives Mastery, not Haste
  if ( !talent.subtlety.flagellation->ok() )
  {
    if ( buffs.flagellation->check() )
    {
      h *= 1.0 / ( 1.0 + buffs.flagellation->check_stack_value() );
    }
    if ( buffs.flagellation_persist->check() )
    {
      h *= 1.0 / ( 1.0 + buffs.flagellation_persist->check_stack_value() );
    }
  }

  return h;
}

// rogue_t::composite_damage_versatility ===================================

double rogue_t::composite_damage_versatility() const
{
  double cdv = player_t::composite_damage_versatility();

  // DFALPHA TOCHECK -- Currently Obedience does nothing on the talent version
  if ( legendary.obedience->ok() && !talent.subtlety.flagellation->ok() )
  {
    if ( buffs.flagellation->check() )
    {
      cdv += buffs.flagellation->check() * buffs.flagellation->data().effectN( 5 ).percent();
    }
    if ( buffs.flagellation_persist->check() )
    {
      // Use the base buff effect since the persist default_value is passed from the base default_value
      cdv += buffs.flagellation_persist->check() * buffs.flagellation->data().effectN( 5 ).percent();
    }
  }

  cdv += talent.rogue.thiefs_versatility->effectN( 1 ).percent();

  return cdv;
}

// rogue_t::composite_heal_versatility =====================================

double rogue_t::composite_heal_versatility() const
{
  double chv = player_t::composite_heal_versatility();

  // DFALPHA TOCHECK -- Currently Obedience does nothing on the talent version
  if ( legendary.obedience->ok() && !talent.subtlety.flagellation->ok() && buffs.flagellation->check() )
  {
    chv += buffs.flagellation->check_stack_value();
  }

  chv += talent.rogue.thiefs_versatility->effectN( 1 ).percent();

  return chv;
}

// rogue_t::composite_leech ===============================================

double rogue_t::composite_leech() const
{
  double l = player_t::composite_leech();

  if ( buffs.grand_melee->check() )
  {
    l += buffs.grand_melee->data().effectN( 2 ).percent();
  }

  l += spell.leeching_poison_buff->effectN( 1 ).percent();

  return l;
}

// rogue_t::matching_gear_multiplier ========================================

double rogue_t::matching_gear_multiplier( attribute_e attr ) const
{
  if ( attr == ATTR_AGILITY )
    return 0.05;

  return 0.0;
}

// rogue_t::composite_player_multiplier =====================================

double rogue_t::composite_player_multiplier( school_e school ) const
{
  double m = player_t::composite_player_multiplier( school );

  if ( legendary.guile_charm.ok() )
  {
    m *= 1.0 + buffs.guile_charm_insight_1->value();
    m *= 1.0 + buffs.guile_charm_insight_2->value();
    m *= 1.0 + buffs.guile_charm_insight_3->value();
  }

  if ( legendary.celerity.ok() && buffs.adrenaline_rush->check() )
  {
    m *= 1.0 + legendary.celerity->effectN( 1 ).percent();
  }

  if ( talent.subtlety.veiltouched->effectN( 1 ).has_common_school( school ) )
  {
    m *= 1.0 + talent.subtlety.veiltouched->effectN( 1 ).percent();
  }

  if ( talent.subtlety.dark_brew->effectN( 2 ).has_common_school( school ) )
  {
    m *= 1.0 + talent.subtlety.dark_brew->effectN( 2 ).percent();
  }

  // Dragonflight version of Deeper Daggers is not a whitelisted buff and still a school buff
  if ( talent.subtlety.deeper_daggers->ok() &&
       spec.deeper_daggers_buff->effectN( 1 ).has_common_school( school ) )
  {
    m *= buffs.deeper_daggers->value_direct();
  }

  return m;
}

// rogue_t::composite_player_target_multiplier ==============================

double rogue_t::composite_player_target_multiplier( player_t* target, school_e school ) const
{
  double m = player_t::composite_player_target_multiplier( target, school );

  rogue_td_t* tdata = get_target_data( target );

  m *= 1.0 + tdata->debuffs.ghostly_strike->stack_value();
  m *= 1.0 + tdata->debuffs.prey_on_the_weak->stack_value();

  return m;
}

// rogue_t::composite_player_target_crit_chance =============================

double rogue_t::composite_player_target_crit_chance( player_t* target ) const
{
  double c = player_t::composite_player_target_crit_chance( target );

  auto td = get_target_data( target );

  c += td->debuffs.between_the_eyes->stack_value();

  return c;
}

// rogue_t::composite_player_target_armor ===================================

double rogue_t::composite_player_target_armor( player_t* target ) const
{
  double a = player_t::composite_player_target_armor( target );

  a *= 1.0 - get_target_data( target )->debuffs.find_weakness->value();

  return a;
}

// rogue_t::default_flask ===================================================

std::string rogue_t::default_flask() const
{
  return ( true_level >= 51 ) ? "spectral_flask_of_power" :
         ( true_level >= 40 ) ? "greater_flask_of_the_currents" :
         ( true_level >= 35 ) ? "greater_draenic_agility_flask" :
         "disabled";
}

// rogue_t::default_potion ==================================================

std::string rogue_t::default_potion() const
{
  return ( true_level >= 51 ) ? "potion_of_spectral_agility" :
         ( true_level >= 40 ) ? "potion_of_unbridled_fury" :
         ( true_level >= 35 ) ? "draenic_agility" :
         "disabled";
}

// rogue_t::default_food ====================================================

std::string rogue_t::default_food() const
{
  return ( true_level >= 51 ) ? "feast_of_gluttonous_hedonism" :
         ( true_level >= 45 ) ? "famine_evaluator_and_snack_table" :
         ( true_level >= 40 ) ? "lavish_suramar_feast" :
         "disabled";
}

// rogue_t::default_rune ====================================================

std::string rogue_t::default_rune() const
{
  return ( true_level >= 60 ) ? "veiled" :
         ( true_level >= 50 ) ? "battle_scarred" :
         ( true_level >= 45 ) ? "defiled" :
         ( true_level >= 40 ) ? "hyper" :
         "disabled";
}

// rogue_t::default_temporary_enchant =======================================

std::string rogue_t::default_temporary_enchant() const
{
  return true_level >= 60 ? "main_hand:shaded_sharpening_stone/off_hand:shaded_sharpening_stone"
    :                       "disabled";
}

// rogue_t::init_actions ====================================================

void rogue_t::init_action_list()
{
  if ( main_hand_weapon.type == WEAPON_NONE )
  {
    if ( !quiet )
      sim->errorf( "Player %s has no weapon equipped at the Main-Hand slot.", name() );
    quiet = true;
    return;
  }

  if ( !action_list_str.empty() )
  {
    player_t::init_action_list();
    return;
  }

  action_priority_list_t* precombat = get_action_priority_list( "precombat" );
  action_priority_list_t* def       = get_action_priority_list( "default" );

  std::vector<std::string> racial_actions = get_racial_actions();

  clear_action_priority_lists();

  // Buffs
  precombat->add_action( "apply_poison" );
  precombat->add_action( "flask" );
  precombat->add_action( "augmentation" );
  precombat->add_action( "food" );
  precombat->add_action( "snapshot_stats", "Snapshot raid buffed stats before combat begins and pre-potting is done." );

  // Potions
  std::string potion_action = "potion,if=buff.bloodlust.react|fight_remains<30";
  if ( specialization() == ROGUE_ASSASSINATION )
    potion_action += "|debuff.deathmark.up";
  else if ( specialization() == ROGUE_OUTLAW )
    potion_action += "|buff.adrenaline_rush.up";
  else if ( specialization() == ROGUE_SUBTLETY )
    potion_action += "|buff.symbols_of_death.up&(buff.shadow_blades.up|cooldown.shadow_blades.remains<=10)";

  // Pre-Combat MfD
  if ( specialization() == ROGUE_ASSASSINATION )
    precombat->add_action( "marked_for_death,precombat_seconds=10,if=!covenant.venthyr&raid_event.adds.in>15");
  if ( specialization() == ROGUE_OUTLAW )
    precombat->add_action( "marked_for_death,precombat_seconds=10,if=raid_event.adds.in>25");

  // Pre-Combat Fleshcraft for Pustule Eruption
  precombat->add_action( "fleshcraft,if=soulbind.pustule_eruption|soulbind.volatile_solvent" );

  // Make restealth first action in the default list.
  def->add_action( "stealth", "Restealth if possible (no vulnerable enemies in combat)" );
  def->add_action( "kick", "Interrupt on cooldown to allow simming interactions with that" );

  if ( specialization() == ROGUE_ASSASSINATION )
  {
    // Pre-Combat
    precombat->add_action( "variable,name=deathmark_cdr,value=1-(runeforge.duskwalkers_patch*0.45)" );
    precombat->add_action( "variable,name=flagellation_cdr,value=1-(runeforge.obedience*0.44)", "The average CDR is 0.22 but due to the RNG nature of CP gen, 2x this value is optimal for syncing logic" );
    precombat->add_action( "variable,name=trinket_sync_slot,value=1,if=trinket.1.has_stat.any_dps&(!trinket.2.has_stat.any_dps|trinket.1.cooldown.duration>=trinket.2.cooldown.duration)|trinket.1.is.inscrutable_quantum_device|(covenant.venthyr&!trinket.2.has_stat.any_dps&trinket.1.is.shadowgrasp_totem)", "Determine which (if any) stat buff trinket we want to attempt to sync with Deathmark." );
    precombat->add_action( "variable,name=trinket_sync_slot,value=2,if=trinket.2.has_stat.any_dps&(!trinket.1.has_stat.any_dps|trinket.2.cooldown.duration>trinket.1.cooldown.duration)|trinket.2.is.inscrutable_quantum_device|(covenant.venthyr&!trinket.1.has_stat.any_dps&trinket.2.is.shadowgrasp_totem)" );
    precombat->add_action( "stealth" );
    precombat->add_action( "slice_and_dice,precombat_seconds=1");

    // Main Rotation
    def->add_action( "variable,name=single_target,value=spell_targets.fan_of_knives<2" );
    def->add_action( "variable,name=regen_saturated,value=energy.regen_combined>35", "Combined Energy Regen needed to saturate" );
    def->add_action( "variable,name=deathmark_cooldown_remains,value=cooldown.deathmark.remains*variable.deathmark_cdr" );
    def->add_action( "call_action_list,name=stealthed,if=stealthed.rogue|stealthed.improved_garrote" );
    def->add_action( "call_action_list,name=cds" );
    def->add_action( "slice_and_dice,if=!buff.slice_and_dice.up&combo_points>=2", "Put SnD up initially for Cut to the Chase, refresh with Envenom if at low duration" );
    def->add_action( "envenom,if=buff.slice_and_dice.up&buff.slice_and_dice.remains<5&combo_points>=4", "Higher priority Envenom casts for refreshing SnD or at the end of Flagellation");
    def->add_action( "envenom,if=buff.flagellation_buff.up&buff.flagellation_buff.remains<1&buff.flagellation_buff.stack<30&combo_points>=2" );
    def->add_action( "call_action_list,name=dot" );
    def->add_action( "call_action_list,name=direct" );
    def->add_action( "arcane_torrent,if=energy.deficit>=15+energy.regen_combined" );
    def->add_action( "arcane_pulse" );
    def->add_action( "lights_judgment" );
    def->add_action( "bag_of_tricks" );

    // Cooldowns
    action_priority_list_t* cds = get_action_priority_list( "cds", "Cooldowns" );
    cds->add_action( "marked_for_death,line_cd=1.5,target_if=min:target.time_to_die,if=raid_event.adds.up&(!variable.single_target|target.time_to_die<30)&(target.time_to_die<combo_points.deficit*1.5|combo_points.deficit>=cp_max_spend)", "If adds are up, snipe the one with lowest TTD. Use when dying faster than CP deficit or without any CP.");
    cds->add_action( "marked_for_death,if=raid_event.adds.in>30-raid_event.adds.duration&combo_points.deficit>=cp_max_spend&(!covenant.venthyr|(buff.flagellation_buff.up|cooldown.flagellation.remains>15)&(talent.crimson_tempest.enabled|!cooldown.shiv.ready))", "If no adds will die within the next 30s, use MfD for max CP. Attempt to sync with Flagellation if possible." );
    cds->add_action( "variable,name=deathmark_exsanguinate_condition,value=!talent.exsanguinate|cooldown.exsanguinate.remains>15|exsanguinated.rupture|exsanguinated.garrote", "Sync Deathmark window with Exsanguinate if applicable" );
    cds->add_action( "variable,name=deathmark_ma_condition,value=!talent.master_assassin.enabled|dot.garrote.ticking|covenant.venthyr&combo_points.deficit=0", "Wait on Deathmark for Garrote with MA, unless we are at max CP for Flagellation" );
    cds->add_action( "variable,name=deathmark_covenant_condition,if=!covenant.venthyr,value=1", "Sync Deathmark with Flagellation as long as we won't lose a cast over the fight duration" );
    cds->add_action( "variable,name=deathmark_covenant_condition,if=covenant.venthyr,value=floor((fight_remains-20)%(120*variable.deathmark_cdr))>floor((fight_remains-20-cooldown.flagellation.remains)%(120*variable.deathmark_cdr))&cooldown.flagellation.remains>10|buff.flagellation_buff.up|debuff.flagellation.up|fight_remains<20" );
    cds->add_action( "fleshcraft,if=(soulbind.pustule_eruption|soulbind.volatile_solvent)&!stealthed.all&!debuff.deathmark.up&master_assassin_remains=0&(energy.time_to_max_combined>2|!debuff.shiv.up)", "Fleshcraft for Pustule Eruption if not stealthed or in a cooldown cycle" );
    cds->add_action( "flagellation,if=!stealthed.rogue&(variable.deathmark_cooldown_remains<3&variable.deathmark_ma_condition&effective_combo_points>=4&target.time_to_die>10|debuff.deathmark.up|fight_remains<24)", "Sync Flagellation with Deathmark as long as we won't lose a cast over the fight duration" );
    cds->add_action( "flagellation,if=!stealthed.rogue&effective_combo_points>=4&(floor((fight_remains-24)%(cooldown*variable.flagellation_cdr))>floor((fight_remains-24-variable.deathmark_cooldown_remains)%(cooldown*variable.flagellation_cdr)))" );
    cds->add_action( "sepsis,if=!stealthed.rogue&dot.garrote.ticking&(target.time_to_die>10|fight_remains<10)" );
    cds->add_action( "variable,name=deathmark_condition,value=!stealthed.rogue&dot.rupture.ticking&!debuff.deathmark.up&variable.deathmark_exsanguinate_condition&variable.deathmark_ma_condition&variable.deathmark_covenant_condition", "Deathmark to be used if not stealthed, Rupture is up, and all other talent/covenant conditions are satisfied");
    cds->add_action( "use_item,name=wraps_of_electrostatic_potential" );
    cds->add_action( "use_item,name=ring_of_collapsing_futures,if=buff.temptation.down|fight_remains<30" );
    cds->add_action( "use_items,slots=trinket1,if=(variable.trinket_sync_slot=1&(debuff.deathmark.up|fight_remains<=20)|(variable.trinket_sync_slot=2&(!trinket.2.cooldown.ready|variable.deathmark_cooldown_remains>20))|!variable.trinket_sync_slot)", "Sync the priority stat buff trinket with Deathmark, otherwise use on cooldown" );
    cds->add_action( "use_items,slots=trinket2,if=(variable.trinket_sync_slot=2&(debuff.deathmark.up|fight_remains<=20)|(variable.trinket_sync_slot=1&(!trinket.1.cooldown.ready|variable.deathmark_cooldown_remains>20))|!variable.trinket_sync_slot)" );
    cds->add_action( "deathmark,if=variable.deathmark_condition" );
    cds->add_action( "kingsbane,if=(debuff.shiv.up|cooldown.shiv.remains<6)&buff.envenom.up&(cooldown.deathmark.remains>=50|dot.deathmark.ticking)" );
    cds->add_action( "exsanguinate,if=!stealthed.rogue&!stealthed.improved_garrote&!dot.deathmark.ticking&(!dot.garrote.refreshable&dot.rupture.remains>4+4*cp_max_spend|dot.rupture.remains*0.5>target.time_to_die)&target.time_to_die>4", "Exsanguinate when not stealthed and both Rupture and Garrote are up for long enough." );
    cds->add_action( "shiv,if=talent.kingsbane&!debuff.shiv.up&dot.kingsbane.ticking&dot.garrote.ticking&dot.rupture.ticking&(!talent.crimson_tempest.enabled|variable.single_target|dot.crimson_tempest.ticking)", "Shiv if DoTs are up; if Night Fae attempt to sync with Sepsis or Deathmark if we won't waste more than half Shiv's cooldown" );
    cds->add_action( "shiv,if=!talent.kingsbane&!covenant.night_fae&!debuff.shiv.up&dot.garrote.ticking&dot.rupture.ticking&(!talent.crimson_tempest.enabled|variable.single_target|dot.crimson_tempest.ticking)" );
    cds->add_action( "shiv,if=!talent.kingsbane&covenant.night_fae&!debuff.shiv.up&dot.garrote.ticking&dot.rupture.ticking&((cooldown.sepsis.ready|cooldown.sepsis.remains>12)+(cooldown.deathmark.ready|variable.deathmark_cooldown_remains>12)=2)");
    cds->add_action( "thistle_tea,if=energy.deficit>=100&!buff.thistle_tea.up&(charges=3|debuff.deathmark.up|fight_remains<cooldown.deathmark.remains)");
    cds->add_action( "indiscriminate_carnage,if=(spell_targets.fan_of_knives>desired_targets|spell_targets.fan_of_knives>1&raid_event.adds.in>60)&(!talent.improved_garrote|cooldown.vanish.remains>45)" );

    // Non-spec stuff with lower prio
    cds->add_action( potion_action );
    cds->add_action( "blood_fury,if=debuff.deathmark.up" );
    cds->add_action( "berserking,if=debuff.deathmark.up" );
    cds->add_action( "fireblood,if=debuff.deathmark.up" );
    cds->add_action( "ancestral_call,if=debuff.deathmark.up" );
    cds->add_action( "call_action_list,name=vanish,if=!stealthed.all&master_assassin_remains=0" );
    cds->add_action( "use_item,name=windscar_whetstone,if=spell_targets.fan_of_knives>desired_targets|raid_event.adds.in>60|fight_remains<7" );
    cds->add_action( "use_item,name=cache_of_acquired_treasures,if=buff.acquired_axe.up&(spell_targets.fan_of_knives=1&raid_event.adds.in>60|spell_targets.fan_of_knives>1)|fight_remains<25" );
    cds->add_action( "use_item,name=bloodstained_handkerchief,target_if=max:target.time_to_die*(!dot.cruel_garrote.ticking),if=!dot.cruel_garrote.ticking" );
    cds->add_action( "use_item,name=scars_of_fraternal_strife,if=!buff.scars_of_fraternal_strife_4.up|fight_remains<30" );

    // Vanish
    action_priority_list_t* vanish = get_action_priority_list( "vanish", "Vanish" );
    vanish->add_action( "pool_resource,for_next=1,extra_amount=45", "Vanish Sync for Improved Garrote with Deathmark" );
    vanish->add_action( "vanish,if=talent.improved_garrote&cooldown.garrote.up&!exsanguinated.garrote&dot.garrote.pmultiplier<=1&(debuff.deathmark.up|cooldown.deathmark.remains<4)&combo_points.deficit>=(spell_targets.fan_of_knives>?4)" );
    vanish->add_action( "pool_resource,for_next=1,extra_amount=45", "Vanish for Indiscriminate Carnage or Improved Garrote at 2-3+ targets");
    vanish->add_action( "vanish,if=talent.improved_garrote&cooldown.garrote.up&!exsanguinated.garrote&dot.garrote.pmultiplier<=1&spell_targets.fan_of_knives>(3-talent.indiscriminate_carnage)&(!talent.indiscriminate_carnage|cooldown.indiscriminate_carnage.ready)" );
    vanish->add_action( "vanish,if=!talent.improved_garrote&(talent.master_assassin.enabled|runeforge.mark_of_the_master_assassin)&!dot.rupture.refreshable&dot.garrote.remains>3&debuff.deathmark.up&(debuff.shiv.up|debuff.deathmark.remains<4|dot.sepsis.ticking)&dot.sepsis.remains<3", "Vanish with Master Assasin: Rupture+Garrote not in refresh range, during Deathmark+Shiv. Sync with Sepsis final hit if possible." );
    vanish->add_action( "pool_resource,for_next=1,extra_amount=45" );
    vanish->add_action( "shadow_dance,if=talent.improved_garrote&cooldown.garrote.up&!exsanguinated.garrote&dot.garrote.pmultiplier<=1&(debuff.deathmark.up|cooldown.deathmark.remains<4|cooldown.deathmark.remains>60)&combo_points.deficit>=(spell_targets.fan_of_knives>?4)" );
    vanish->add_action( "shadow_dance,if=!talent.improved_garrote&(talent.master_assassin.enabled|runeforge.mark_of_the_master_assassin)&!dot.rupture.refreshable&dot.garrote.remains>3&(debuff.deathmark.up|cooldown.deathmark.remains>60)&(debuff.shiv.up|debuff.deathmark.remains<4|dot.sepsis.ticking)&dot.sepsis.remains<3", "Shadow Dance with Master Assasin: Rupture+Garrote not in refresh range, during Deathmark+Shiv. Sync with Sepsis final hit if possible." );

    // Stealth
    action_priority_list_t* stealthed = get_action_priority_list( "stealthed", "Stealthed Actions" );
    stealthed->add_action( "indiscriminate_carnage,if=spell_targets.fan_of_knives>desired_targets|spell_targets.fan_of_knives>1&raid_event.adds.in>60" );
    stealthed->add_action( "pool_resource,for_next=1", "Improved Garrote: Apply or Refresh with buffed Garrotes" );
    stealthed->add_action( "garrote,target_if=min:remains,if=stealthed.improved_garrote&!will_lose_exsanguinate&(remains<12%exsanguinated_rate|pmultiplier<=1)&target.time_to_die-remains>2");
    stealthed->add_action( "pool_resource,for_next=1", "Improved Garrote + Exsg on 1T: Refresh Garrote at the end of stealth to get max duration before Exsanguinate" );
    stealthed->add_action( "garrote,if=talent.exsanguinate.enabled&stealthed.improved_garrote&active_enemies=1&!will_lose_exsanguinate&improved_garrote_remains<1.3");

    // Damage over time abilities
    action_priority_list_t* dot = get_action_priority_list( "dot", "Damage over time abilities" );
    dot->add_action( "variable,name=skip_cycle_garrote,value=priority_rotation&(dot.garrote.remains<cooldown.garrote.duration|variable.regen_saturated)", "Limit secondary Garrotes for priority rotation if we have 35 energy regen or Garrote will expire on the primary target" );
    dot->add_action( "variable,name=skip_cycle_rupture,value=priority_rotation&(debuff.shiv.up&spell_targets.fan_of_knives>2|variable.regen_saturated)", "Limit secondary Ruptures for priority rotation if we have 35 energy regen or Shiv is up on 2T+" );
    dot->add_action( "variable,name=skip_rupture,value=debuff.deathmark.up&(debuff.shiv.up|master_assassin_remains>0)&dot.rupture.remains>2", "Limit Ruptures if Deathmark+Shiv/Master Assassin is up and we have 2+ seconds left on the Rupture DoT" );
    dot->add_action( "garrote,if=talent.exsanguinate.enabled&!will_lose_exsanguinate&dot.garrote.pmultiplier<=1&cooldown.exsanguinate.remains<2&spell_targets.fan_of_knives=1&raid_event.adds.in>6&dot.garrote.remains*0.5<target.time_to_die", "Special Garrote and Rupture setup prior to Exsanguinate cast");
    dot->add_action( "rupture,if=talent.exsanguinate.enabled&!will_lose_exsanguinate&dot.rupture.pmultiplier<=1&(effective_combo_points>=cp_max_spend&cooldown.exsanguinate.remains<1&dot.rupture.remains*0.5<target.time_to_die)");
    dot->add_action( "pool_resource,for_next=1", "Garrote upkeep, also tries to use it as a special generator for the last CP before a finisher" );
    dot->add_action( "garrote,if=refreshable&combo_points.deficit>=1&(pmultiplier<=1|remains<=tick_time&spell_targets.fan_of_knives>=3)&(!will_lose_exsanguinate|remains<=tick_time*2&spell_targets.fan_of_knives>=3)&(target.time_to_die-remains)>4&master_assassin_remains=0" );
    dot->add_action( "pool_resource,for_next=1" );
    dot->add_action( "garrote,cycle_targets=1,if=!variable.skip_cycle_garrote&target!=self.target&refreshable&combo_points.deficit>=1&(pmultiplier<=1|remains<=tick_time&spell_targets.fan_of_knives>=3)&(!will_lose_exsanguinate|remains<=tick_time*2&spell_targets.fan_of_knives>=3)&(target.time_to_die-remains)>12&master_assassin_remains=0" );
    dot->add_action( "crimson_tempest,target_if=min:remains,if=spell_targets>=2&effective_combo_points>=4&energy.regen_combined>20&(!cooldown.deathmark.ready|dot.rupture.ticking)&remains<(2+3*(spell_targets>=4))", "Crimson Tempest on multiple targets at 4+ CP when running out in 2-5s as long as we have enough regen and aren't setting up for Deathmark");
    dot->add_action( "rupture,if=!variable.skip_rupture&effective_combo_points>=4&refreshable&(pmultiplier<=1|remains<=tick_time&spell_targets.fan_of_knives>=3)&(!will_lose_exsanguinate|remains<=tick_time*2&spell_targets.fan_of_knives>=3)&target.time_to_die-remains>(4+(runeforge.dashing_scoundrel*5)+(runeforge.doomblade*5)+(variable.regen_saturated*6))", "Keep up Rupture at 4+ on all targets (when living long enough and not snapshot)" );
    dot->add_action( "rupture,cycle_targets=1,if=!variable.skip_cycle_rupture&!variable.skip_rupture&target!=self.target&effective_combo_points>=4&refreshable&(pmultiplier<=1|remains<=tick_time&spell_targets.fan_of_knives>=3)&(!will_lose_exsanguinate|remains<=tick_time*2&spell_targets.fan_of_knives>=3)&target.time_to_die-remains>(4+(runeforge.dashing_scoundrel*5)+(runeforge.doomblade*5)+(variable.regen_saturated*6))" );
    dot->add_action( "crimson_tempest,if=spell_targets>=2&effective_combo_points>=4&remains<2+3*(spell_targets>=4)", "Fallback AoE Crimson Tempest with the same logic as above, but ignoring the energy conditions if we aren't using Rupture" );
    dot->add_action( "crimson_tempest,if=spell_targets=1&(!runeforge.dashing_scoundrel|rune_word.frost.enabled)&effective_combo_points>=(cp_max_spend-1)&refreshable&!will_lose_exsanguinate&!debuff.shiv.up&debuff.amplifying_poison.stack<15&target.time_to_die-remains>4", "Crimson Tempest on ST if in pandemic and nearly max energy and if Envenom won't do more damage due to TB/MA" );

    // Direct damage abilities
    action_priority_list_t* direct = get_action_priority_list( "direct", "Direct damage abilities" );
    direct->add_action( "envenom,if=effective_combo_points>=4+talent.deeper_stratagem.enabled&(debuff.deathmark.up|debuff.shiv.up|debuff.amplifying_poison.stack>=10|buff.flagellation_buff.up|energy.deficit<=25+energy.regen_combined|!variable.single_target|effective_combo_points>cp_max_spend)&(!talent.exsanguinate.enabled|cooldown.exsanguinate.remains>2)", "Envenom at 4+ (5+ with DS) CP. Immediately on 2+ targets, with Deathmark, or with TB; otherwise wait for some energy. Also wait if Exsg combo is coming up.");
    direct->add_action( "variable,name=use_filler,value=combo_points.deficit>1|energy.deficit<=25+energy.regen_combined|!variable.single_target" );
    direct->add_action( "serrated_bone_spike,if=variable.use_filler&!dot.serrated_bone_spike_dot.ticking", "Apply SBS to all targets without a debuff as priority, preferring targets dying sooner after the primary target" );
    direct->add_action( "serrated_bone_spike,target_if=min:target.time_to_die+(dot.serrated_bone_spike_dot.ticking*600),if=variable.use_filler&!dot.serrated_bone_spike_dot.ticking" );
    direct->add_action( "serrated_bone_spike,if=variable.use_filler&master_assassin_remains<0.8&(fight_remains<=5|cooldown.serrated_bone_spike.max_charges-charges_fractional<=0.25)", "Keep from capping charges or burn at the end of fights" );
    direct->add_action( "serrated_bone_spike,if=variable.use_filler&master_assassin_remains<0.8&(soulbind.lead_by_example.enabled&!buff.lead_by_example.up&debuff.deathmark.up|buff.marrowed_gemstone_enhancement.up|!variable.single_target&debuff.shiv.up)", "When MA is not at high duration, sync with damage buffs without overwriting Lead by Example" );
    direct->add_action( "fan_of_knives,if=variable.use_filler&(!priority_rotation&spell_targets.fan_of_knives>=3+stealthed.rogue+talent.dragontempered_blades)", "Fan of Knives at 19+ stacks of Hidden Blades or against 4+ targets." );
    direct->add_action( "fan_of_knives,target_if=!dot.deadly_poison_dot.ticking&(!priority_rotation|dot.garrote.ticking|dot.rupture.ticking),if=variable.use_filler&spell_targets.fan_of_knives>=3", "Fan of Knives to apply poisons if inactive on any target (or any bleeding targets with priority rotation) at 3T" );
    direct->add_action( "echoing_reprimand,if=variable.use_filler&variable.deathmark_cooldown_remains>10" );
    direct->add_action( "ambush,if=variable.use_filler&(master_assassin_remains=0&!runeforge.doomblade|buff.blindside.up)" );
    direct->add_action( "mutilate,target_if=!dot.deadly_poison_dot.ticking,if=variable.use_filler&spell_targets.fan_of_knives=2", "Tab-Mutilate to apply Deadly Poison at 2 targets");
    direct->add_action( "mutilate,if=variable.use_filler" );
  }
  else if ( specialization() == ROGUE_OUTLAW )
  {
    // Pre-Combat
    precombat->add_action( "adrenaline_rush,precombat_seconds=3,if=talent.improved_adrenaline_rush" );
    precombat->add_action( "roll_the_bones,precombat_seconds=2");
    precombat->add_action( "slice_and_dice,precombat_seconds=1" );
    precombat->add_action( "stealth" );

    // Main Rotation
    def->add_action( "variable,name=rtb_reroll,value=rtb_buffs<2&(!buff.broadside.up&(!runeforge.concealed_blunderbuss&!talent.fan_the_hammer|!buff.skull_and_crossbones.up)&(!runeforge.invigorating_shadowdust|!buff.true_bearing.up))|rtb_buffs=2&buff.buried_treasure.up&buff.grand_melee.up", "Reroll BT + GM or single buffs early other than Broadside, TB with Shadowdust, or SnC with Blunderbuss" );
    def->add_action( "variable,name=ambush_condition,value=combo_points.deficit>=2+buff.broadside.up&energy>=50&(!conduit.count_the_odds&!talent.count_the_odds|buff.roll_the_bones.remains>=10)", "Ensure we get full Ambush CP gains and aren't rerolling Count the Odds buffs away" );
    def->add_action( "variable,name=finish_condition,value=combo_points>=cp_max_spend-buff.broadside.up-(buff.opportunity.up*(talent.quick_draw|talent.fan_the_hammer)|buff.concealed_blunderbuss.up)|effective_combo_points>=cp_max_spend", "Finish at max possible CP without overflowing bonus combo points, unless for BtE which always should be 5+ CP" );
    def->add_action( "variable,name=finish_condition,op=reset,if=cooldown.between_the_eyes.ready&effective_combo_points<5", "Always attempt to use BtE at 5+ CP, regardless of CP gen waste" );
    def->add_action( "variable,name=finish_condition,value=1,if=buff.flagellation_buff.up&buff.flagellation_buff.remains<1&effective_combo_points>=2", "Finish at 2+ in the last GCD of Flagellation" );
    def->add_action( "variable,name=blade_flurry_sync,value=spell_targets.blade_flurry<2&raid_event.adds.in>20|buff.blade_flurry.remains>1+talent.killing_spree.enabled", "With multiple targets, this variable is checked to decide whether some CDs should be synced with Blade Flurry" );
    def->add_action( "run_action_list,name=stealth,if=stealthed.all" );
    def->add_action( "call_action_list,name=cds" );
    def->add_action( "run_action_list,name=finish,if=variable.finish_condition" );
    def->add_action( "call_action_list,name=build" );
    def->add_action( "arcane_torrent,if=energy.base_deficit>=15+energy.regen" );
    def->add_action( "arcane_pulse" );
    def->add_action( "lights_judgment" );
    def->add_action( "bag_of_tricks" );

    // Cooldowns
    action_priority_list_t* cds = get_action_priority_list( "cds", "Cooldowns" );
    cds->add_action( "blade_flurry,if=spell_targets>=2&!buff.blade_flurry.up", "Blade Flurry on 2+ enemies");
    cds->add_action( "roll_the_bones,if=master_assassin_remains=0&buff.dreadblades.down&(!buff.roll_the_bones.up|variable.rtb_reroll)");
    cds->add_action( "keep_it_rolling,if=!variable.rtb_reroll&(buff.broadside.up+buff.true_bearing.up+buff.skull_and_crossbones.up+buff.ruthless_precision.up)>2" );
    cds->add_action( "flagellation,target_if=max:target.time_to_die,if=!stealthed.all&(variable.finish_condition&target.time_to_die>10|fight_remains<13)" );
    cds->add_action( "shadow_dance,if=!runeforge.mark_of_the_master_assassin&!runeforge.invigorating_shadowdust&!runeforge.deathly_shadows&!stealthed.all&!buff.take_em_by_surprise.up&(variable.finish_condition&buff.slice_and_dice.up|variable.ambush_condition&!buff.slice_and_dice.up)" );
    cds->add_action( "vanish,if=!runeforge.mark_of_the_master_assassin&!runeforge.invigorating_shadowdust&!runeforge.deathly_shadows&!stealthed.all&!buff.take_em_by_surprise.up&(variable.finish_condition&buff.slice_and_dice.up|variable.ambush_condition&!buff.slice_and_dice.up)" );
    cds->add_action( "vanish,if=runeforge.deathly_shadows&!stealthed.all&buff.deathly_shadows.down&combo_points<=2&variable.ambush_condition", "With Deathly Shadows, optimize for combo point generation when the buff is down");
    cds->add_action( "variable,name=vanish_ma_condition,if=runeforge.mark_of_the_master_assassin&!talent.marked_for_death.enabled,value=(!cooldown.between_the_eyes.ready&variable.finish_condition)|(cooldown.between_the_eyes.ready&variable.ambush_condition)", "With Master Asssassin, sync Vanish with a finisher or Ambush depending on BtE cooldown, or always a finisher with MfD" );
    cds->add_action( "variable,name=vanish_ma_condition,if=runeforge.mark_of_the_master_assassin&talent.marked_for_death.enabled,value=variable.finish_condition" );
    cds->add_action( "vanish,if=variable.vanish_ma_condition&master_assassin_remains=0&variable.blade_flurry_sync");
    cds->add_action( "adrenaline_rush,if=!buff.adrenaline_rush.up&(!talent.improved_adrenaline_rush|combo_points<=2)");
    cds->add_action( "fleshcraft,if=(soulbind.pustule_eruption|soulbind.volatile_solvent)&!stealthed.all&(!buff.blade_flurry.up|spell_targets.blade_flurry<2)&(!buff.adrenaline_rush.up|energy.base_time_to_max>2)", "Fleshcraft for Pustule Eruption if not stealthed and not with Blade Flurry" );
    cds->add_action( "dreadblades,if=!stealthed.all&combo_points<=2&(!covenant.venthyr|buff.flagellation_buff.up)&(!talent.marked_for_death|!cooldown.marked_for_death.ready)");
    cds->add_action( "marked_for_death,line_cd=1.5,target_if=min:target.time_to_die,if=raid_event.adds.up&(target.time_to_die<combo_points.deficit|!stealthed.rogue&combo_points.deficit>=cp_max_spend-1)", "If adds are up, snipe the one with lowest TTD. Use when dying faster than CP deficit or without any CP.");
    cds->add_action( "marked_for_death,if=raid_event.adds.in>30-raid_event.adds.duration&!stealthed.rogue&combo_points.deficit>=cp_max_spend-1&(!covenant.venthyr|cooldown.flagellation.remains>10|buff.flagellation_buff.up)", "If no adds will die within the next 30s, use MfD on boss without any CP." );
    cds->add_action( "variable,name=killing_spree_vanish_sync,value=!runeforge.mark_of_the_master_assassin|cooldown.vanish.remains>10|master_assassin_remains>2", "Attempt to sync Killing Spree with Vanish for Master Assassin" );
    cds->add_action( "killing_spree,if=variable.blade_flurry_sync&variable.killing_spree_vanish_sync&!stealthed.rogue&(debuff.between_the_eyes.up&buff.dreadblades.down&energy.base_deficit>(energy.regen*2+15)|spell_targets.blade_flurry>(2-buff.deathly_shadows.up)|master_assassin_remains>0)", "Use in 1-2T if BtE is up and won't cap Energy, or at 3T+ (2T+ with Deathly Shadows) or when Master Assassin is up.");
    cds->add_action( "blade_rush,if=variable.blade_flurry_sync&(energy.base_time_to_max>2&!buff.dreadblades.up&!buff.flagellation_buff.up|energy<=30|spell_targets>2)");
    cds->add_action( "vanish,if=runeforge.invigorating_shadowdust&covenant.venthyr&!stealthed.all&variable.finish_condition&(!cooldown.flagellation.ready&(!talent.dreadblades|!cooldown.dreadblades.ready|!buff.flagellation_buff.up))", "If using Invigorating Shadowdust, use normal logic in addition to checking major CDs." );
    cds->add_action( "vanish,if=runeforge.invigorating_shadowdust&!covenant.venthyr&!stealthed.all&variable.finish_condition&(cooldown.echoing_reprimand.remains>6|!cooldown.sepsis.ready|cooldown.serrated_bone_spike.full_recharge_time>20)" );
    cds->add_action( "shadowmeld,if=!stealthed.all&((conduit.count_the_odds|talent.count_the_odds)&variable.finish_condition|!talent.weaponmaster.enabled&variable.ambush_condition)" );
    cds->add_action( "thistle_tea,if=energy.deficit>=100&!buff.thistle_tea.up&(charges=3|buff.adrenaline_rush.up|fight_remains<charges*6)" );

    // Non-spec stuff with lower prio
    cds->add_action( potion_action );
    cds->add_action( "blood_fury" );
    cds->add_action( "berserking" );
    cds->add_action( "fireblood" );
    cds->add_action( "ancestral_call" );

    cds->add_action( "use_item,name=wraps_of_electrostatic_potential" );
    cds->add_action( "use_item,name=ring_of_collapsing_futures,if=buff.temptation.down|fight_remains<30" );
    cds->add_action( "use_item,name=windscar_whetstone,if=spell_targets.blade_flurry>desired_targets|raid_event.adds.in>60|fight_remains<7" );
    cds->add_action( "use_item,name=cache_of_acquired_treasures,if=buff.acquired_axe.up|fight_remains<25" );
    cds->add_action( "use_item,name=bloodstained_handkerchief,target_if=max:target.time_to_die*(!dot.cruel_garrote.ticking),if=!dot.cruel_garrote.ticking" );
    cds->add_action( "use_item,name=scars_of_fraternal_strife,if=!buff.scars_of_fraternal_strife_4.up|fight_remains<30" );
    cds->add_action( "use_items,slots=trinket1,if=debuff.between_the_eyes.up|trinket.1.has_stat.any_dps|fight_remains<=20", "Default conditions for usable items." );
    cds->add_action( "use_items,slots=trinket2,if=debuff.between_the_eyes.up|trinket.2.has_stat.any_dps|fight_remains<=20" );

    // Stealth
    action_priority_list_t* stealth = get_action_priority_list( "stealth", "Stealth" );
    stealth->add_action( "dispatch,if=variable.finish_condition" );
    stealth->add_action( "ambush" );

    // Finishers
    action_priority_list_t* finish = get_action_priority_list( "finish", "Finishers" );
    finish->add_action( "between_the_eyes,if=target.time_to_die>3&(debuff.between_the_eyes.remains<4|(runeforge.greenskins_wickers|talent.greenskins_wickers)&!buff.greenskins_wickers.up|!runeforge.greenskins_wickers&!talent.greenskins_wickers&buff.ruthless_precision.up)", "BtE to keep the Crit debuff up, if RP is up, or for Greenskins, unless the target is about to die.");
    finish->add_action( "slice_and_dice,if=buff.slice_and_dice.remains<fight_remains&refreshable&(!talent.swift_slasher|combo_points>=cp_max_spend)" );
    finish->add_action( "cold_blood,if=!(runeforge.greenskins_wickers|talent.greenskins_wickers)" );
    finish->add_action( "dispatch" );

    // Builders
    action_priority_list_t* build = get_action_priority_list( "build", "Builders" );
    build->add_action( "sepsis,target_if=max:target.time_to_die*debuff.between_the_eyes.up,if=target.time_to_die>11&debuff.between_the_eyes.up|fight_remains<11" );
    build->add_action( "ghostly_strike,if=debuff.ghostly_strike.remains<=3" );
    build->add_action( "shiv,if=runeforge.tiny_toxic_blade" );
    build->add_action( "echoing_reprimand,if=!soulbind.effusive_anima_accelerator|variable.blade_flurry_sync" );
    build->add_action( "ambush" );
    build->add_action( "cold_blood,if=buff.opportunity.up&buff.greenskins_wickers.up|buff.greenskins_wickers.up&buff.greenskins_wickers.remains<1.5", "Use Pistol Shot when buffed by bonuses as a priority" );
    build->add_action( "pistol_shot,if=buff.opportunity.up&(buff.greenskins_wickers.up&!talent.fan_the_hammer|buff.concealed_blunderbuss.up)|buff.greenskins_wickers.up&buff.greenskins_wickers.remains<1.5" );
    build->add_action( "pistol_shot,if=talent.fan_the_hammer&buff.opportunity.up&(buff.opportunity.stack>=buff.opportunity.max_stack|buff.opportunity.remains<2)", "With Fan the Hammer, consume Opportunity at max stacks or if we will get 4+ CP and Dreadblades is not up ");
    build->add_action( "pistol_shot,if=talent.fan_the_hammer&buff.opportunity.up&combo_points.deficit>4&!buff.dreadblades.up" );
    build->add_action( "pool_resource,for_next=1" );
    build->add_action( "ambush" );
    build->add_action( "serrated_bone_spike,if=!dot.serrated_bone_spike_dot.ticking", "Apply SBS to all targets without a debuff as priority, preferring targets dying sooner after the primary target" );
    build->add_action( "serrated_bone_spike,target_if=min:target.time_to_die+(dot.serrated_bone_spike_dot.ticking*600),if=!dot.serrated_bone_spike_dot.ticking" );
    build->add_action( "serrated_bone_spike,if=fight_remains<=5|cooldown.serrated_bone_spike.max_charges-charges_fractional<=0.25|combo_points.deficit=cp_gain&!buff.skull_and_crossbones.up&energy.base_time_to_max>1", "Attempt to use when it will cap combo points and SnD is down, otherwise keep from capping charges" );
    build->add_action( "pistol_shot,if=!talent.fan_the_hammer&buff.opportunity.up&(energy.base_deficit>energy.regen*1.5|!talent.weaponmaster&combo_points.deficit<=1+buff.broadside.up|talent.quick_draw.enabled|talent.audacity.enabled&!buff.audacity.up)", "Use Pistol Shot with Opportunity if Combat Potency won't overcap energy, when it will exactly cap CP, or when using Quick Draw");
    build->add_action( "sinister_strike,target_if=min:dot.vicious_wound.remains,if=buff.acquired_axe_driver.up", "Use Sinister Strike on targets without the Cache DoT if the trinket is up");
    build->add_action( "sinister_strike" );
  }
  else if ( specialization() == ROGUE_SUBTLETY )
  {
    // Pre-Combat
    precombat->add_action( "stealth" );
    precombat->add_action( "marked_for_death,precombat_seconds=15" );
    precombat->add_action( "slice_and_dice,precombat_seconds=1" );
    precombat->add_action( "shadow_blades,if=runeforge.mark_of_the_master_assassin" );

    // Main Rotation
    def->add_action( "variable,name=snd_condition,value=buff.slice_and_dice.up|spell_targets.shuriken_storm>=6", "Used to determine whether cooldowns wait for SnD based on targets." );
    def->add_action( "variable,name=is_next_cp_animacharged,if=covenant.kyrian,value=combo_points=1&buff.echoing_reprimand_2.up|combo_points=2&buff.echoing_reprimand_3.up|combo_points=3&buff.echoing_reprimand_4.up|combo_points=4&buff.echoing_reprimand_5.up", "Check to see if the next CP (in the event of a ShT proc) is Animacharged" );
    def->add_action( "variable,name=effective_combo_points,value=effective_combo_points", "Account for ShT reaction time by ignoring low-CP animacharged matches in the 0.5s preceeding a potential ShT proc" );
    def->add_action( "variable,name=effective_combo_points,if=covenant.kyrian&effective_combo_points>combo_points&combo_points.deficit>2&time_to_sht.4.plus<0.5&!variable.is_next_cp_animacharged,value=combo_points" );
    def->add_action( "call_action_list,name=cds", "Check CDs at first" );
    def->add_action( "slice_and_dice,if=spell_targets.shuriken_storm<6&fight_remains>6&buff.slice_and_dice.remains<gcd.max&combo_points>=4-(time<10)*2", "Apply Slice and Dice at 2+ CP during the first 10 seconds, after that 4+ CP if it expires within the next GCD or is not up" );
    def->add_action( "run_action_list,name=stealthed,if=stealthed.all", "Run fully switches to the Stealthed Rotation (by doing so, it forces pooling if nothing is available)." );
    def->add_action( "variable,name=use_priority_rotation,value=priority_rotation&spell_targets.shuriken_storm>=2", "Only change rotation if we have priority_rotation set and multiple targets up." );
    def->add_action( "call_action_list,name=stealth_cds,if=variable.use_priority_rotation", "Priority Rotation? Let's give a crap about energy for the stealth CDs (builder still respect it). Yup, it can be that simple." );
    def->add_action( "variable,name=stealth_threshold,value=25+talent.vigor.enabled*20+talent.master_of_shadows.enabled*20+talent.shadow_focus.enabled*25+talent.alacrity.enabled*20+25*(spell_targets.shuriken_storm>=4)", "Used to define when to use stealth CDs or builders" );
    def->add_action( "call_action_list,name=stealth_cds,if=energy.deficit<=variable.stealth_threshold", "Consider using a Stealth CD when reaching the energy threshold" );
    def->add_action( "call_action_list,name=finish,if=variable.effective_combo_points>=cp_max_spend" );
    def->add_action( "call_action_list,name=finish,if=combo_points.deficit<=1|fight_remains<=1&variable.effective_combo_points>=3", "Finish at 4+ without DS or with SoD crit buff, 5+ with DS (outside stealth)" );
    def->add_action( "call_action_list,name=finish,if=spell_targets.shuriken_storm>=4&variable.effective_combo_points>=4", "With DS also finish at 4+ against 4 targets (outside stealth)" );
    def->add_action( "call_action_list,name=build,if=energy.deficit<=variable.stealth_threshold", "Use a builder when reaching the energy threshold" );
    def->add_action( "arcane_torrent,if=energy.deficit>=15+energy.regen", "Lowest priority in all of the APL because it causes a GCD" );
    def->add_action( "arcane_pulse" );
    def->add_action( "lights_judgment" );
    def->add_action( "bag_of_tricks" );

    // Cooldowns
    action_priority_list_t* cds = get_action_priority_list( "cds", "Cooldowns" );
    cds->add_action( "shadow_dance,use_off_gcd=1,if=!buff.shadow_dance.up&buff.shuriken_tornado.up&buff.shuriken_tornado.remains<=3.5", "Use Dance off-gcd before the first Shuriken Storm from Tornado comes in.");
    cds->add_action( "symbols_of_death,use_off_gcd=1,if=buff.shuriken_tornado.up&buff.shuriken_tornado.remains<=3.5", "(Unless already up because we took Shadow Focus) use Symbols off-gcd before the first Shuriken Storm from Tornado comes in." );
    cds->add_action( "flagellation,target_if=max:target.time_to_die,if=variable.snd_condition&!stealthed.mantle&(cooldown.shadow_dance.up)&combo_points>=5&target.time_to_die>10" );
    cds->add_action( "vanish,if=(runeforge.mark_of_the_master_assassin&combo_points.deficit<=1-talent.deeper_strategem.enabled|runeforge.deathly_shadows&combo_points<1)&buff.symbols_of_death.up&buff.shadow_dance.up&master_assassin_remains=0&buff.deathly_shadows.down" );
    cds->add_action( "pool_resource,for_next=1,if=talent.shuriken_tornado.enabled&!talent.shadow_focus.enabled", "Pool for Tornado pre-SoD with ShD ready when not running SF." );
    cds->add_action( "shuriken_tornado,if=spell_targets.shuriken_storm<=1&energy>=60&variable.snd_condition&cooldown.symbols_of_death.up&cooldown.shadow_dance.charges>=1&(!runeforge.obedience|buff.flagellation_buff.up|spell_targets.shuriken_storm>=(1+4*(!talent.nightstalker.enabled&!talent.dark_shadow.enabled)))&combo_points<=2&!buff.premeditation.up&(!covenant.venthyr&!talent.flagellation|!cooldown.flagellation.up)", "Use Tornado pre SoD when we have the energy whether from pooling without SF or just generally.");
    cds->add_action( "serrated_bone_spike,cycle_targets=1,if=variable.snd_condition&!dot.serrated_bone_spike_dot.ticking&target.time_to_die>=21&(combo_points.deficit>=(cp_gain>?4))&!buff.shuriken_tornado.up&(!buff.premeditation.up|spell_targets.shuriken_storm>4)|fight_remains<=5&spell_targets.shuriken_storm<3" );
    cds->add_action( "sepsis,if=variable.snd_condition&combo_points.deficit>=1&target.time_to_die>=16" );
    cds->add_action( "symbols_of_death,if=variable.snd_condition&(!stealthed.all|buff.perforated_veins.stack<4|spell_targets.shuriken_storm>4&!variable.use_priority_rotation)&(!talent.shuriken_tornado.enabled|talent.shadow_focus.enabled|spell_targets.shuriken_storm>=2|cooldown.shuriken_tornado.remains>2)&(!covenant.venthyr&!talent.flagellation|cooldown.flagellation.remains>10|cooldown.flagellation.up&combo_points>=5)", "Use Symbols on cooldown (after first SnD) unless we are going to pop Tornado and do not have Shadow Focus.");
    cds->add_action( "marked_for_death,line_cd=1.5,target_if=min:target.time_to_die,if=raid_event.adds.up&(target.time_to_die<combo_points.deficit|!stealthed.all&combo_points.deficit>=cp_max_spend)", "If adds are up, snipe the one with lowest TTD. Use when dying faster than CP deficit or not stealthed without any CP.");
    cds->add_action( "marked_for_death,if=raid_event.adds.in>30-raid_event.adds.duration&combo_points.deficit>=cp_max_spend", "If no adds will die within the next 30s, use MfD on boss without any CP." );
    cds->add_action( "shadow_blades,if=variable.snd_condition&combo_points.deficit>=2&(buff.symbols_of_death.up|fight_remains<=20)");
    cds->add_action( "echoing_reprimand,if=(!talent.shadow_focus.enabled|!stealthed.all|spell_targets.shuriken_storm>=4)&variable.snd_condition&combo_points.deficit>=2&(variable.use_priority_rotation|spell_targets.shuriken_storm<=4|runeforge.resounding_clarity|talent.resounding_clarity)" );
    cds->add_action( "shuriken_tornado,if=(talent.shadow_focus.enabled|spell_targets.shuriken_storm>=2)&variable.snd_condition&buff.symbols_of_death.up&combo_points<=2&(!buff.premeditation.up|spell_targets.shuriken_storm>4)", "With SF, if not already done, use Tornado with SoD up.");
    cds->add_action( "shadow_dance,if=!buff.shadow_dance.up&fight_remains<=8+talent.subterfuge.enabled" );
    cds->add_action( "thistle_tea,if=cooldown.symbols_of_death.remains>=3&!buff.thistle_tea.up&(energy.deficit>=100|cooldown.thistle_tea.charges_fractional>=2.75&energy.deficit>=40)" );
    cds->add_action( "fleshcraft,if=(soulbind.pustule_eruption|soulbind.volatile_solvent)&energy.deficit>=30&!stealthed.all&buff.symbols_of_death.down" );

    // Non-spec stuff with lower prio
    cds->add_action( potion_action );
    cds->add_action( "blood_fury,if=buff.symbols_of_death.up" );
    cds->add_action( "berserking,if=buff.symbols_of_death.up" );
    cds->add_action( "fireblood,if=buff.symbols_of_death.up" );
    cds->add_action( "ancestral_call,if=buff.symbols_of_death.up" );

    cds->add_action( "use_item,name=wraps_of_electrostatic_potential" );
    cds->add_action( "use_item,name=ring_of_collapsing_futures,if=buff.temptation.down|fight_remains<30" );
    cds->add_action( "use_item,name=cache_of_acquired_treasures,if=(covenant.venthyr&buff.acquired_axe.up|!covenant.venthyr&buff.acquired_wand.up)&(spell_targets.shuriken_storm=1&raid_event.adds.in>60|fight_remains<25|variable.use_priority_rotation)|buff.acquired_axe.up&spell_targets.shuriken_storm>1" );
    cds->add_action( "use_item,name=bloodstained_handkerchief,target_if=max:target.time_to_die*(!dot.cruel_garrote.ticking),if=!dot.cruel_garrote.ticking" );
    cds->add_action( "use_item,name=scars_of_fraternal_strife,if=!buff.scars_of_fraternal_strife_4.up|fight_remains<30" );
    cds->add_action( "use_items,if=buff.symbols_of_death.up|fight_remains<20", "Default fallback for usable items: Use with Symbols of Death." );

    // Stealth Cooldowns
    action_priority_list_t* stealth_cds = get_action_priority_list( "stealth_cds", "Stealth Cooldowns" );
    stealth_cds->add_action( "variable,name=shd_threshold,value=cooldown.shadow_dance.charges_fractional>=1.75", "Helper Variable" );
    stealth_cds->add_action( "variable,name=shd_threshold,if=runeforge.the_rotten|talent.the_rotten,value=cooldown.shadow_dance.charges_fractional>=1.75|cooldown.symbols_of_death.remains>=16" );
    stealth_cds->add_action( "vanish,if=(!variable.shd_threshold|!talent.nightstalker.enabled&talent.dark_shadow.enabled)&combo_points.deficit>1&!runeforge.mark_of_the_master_assassin", "Vanish if we are capping on Dance charges. Early before first dance if we have no Nightstalker but Dark Shadow in order to get Rupture up (no Master Assassin).");
    stealth_cds->add_action( "pool_resource,for_next=1,extra_amount=40,if=race.night_elf", "Pool for Shadowmeld + Shadowstrike unless we are about to cap on Dance charges. Only when Find Weakness is about to run out." );
    stealth_cds->add_action( "shadowmeld,if=energy>=40&energy.deficit>=10&!variable.shd_threshold&combo_points.deficit>1" );
    stealth_cds->add_action( "variable,name=shd_combo_points,value=combo_points.deficit>=2+buff.shadow_blades.up", "CP thresholds for entering Shadow Dance" );
    stealth_cds->add_action( "variable,name=shd_combo_points,value=combo_points.deficit>=3,if=covenant.kyrian" );
    stealth_cds->add_action( "variable,name=shd_combo_points,value=combo_points.deficit<=1,if=variable.use_priority_rotation&spell_targets.shuriken_storm>=4" );
    stealth_cds->add_action( "variable,name=shd_combo_points,value=combo_points.deficit<=1,if=spell_targets.shuriken_storm=4" );
    stealth_cds->add_action( "shadow_dance,if=(variable.shd_combo_points&(buff.symbols_of_death.remains>=(2.2-talent.flagellation.enabled)|variable.shd_threshold)|buff.flagellation.up|buff.flagellation_persist.remains>=6|spell_targets.shuriken_storm>=4&cooldown.symbols_of_death.remains>10)&(buff.perforated_veins.stack<4|spell_targets.shuriken_storm>3)&!cooldown.flagellation.up", "Dance during Symbols or above threshold." );
    stealth_cds->add_action( "shadow_dance,if=variable.shd_combo_points&fight_remains<cooldown.symbols_of_death.remains", "Burn Dances charges if the fight ends and SoD won't be ready in time." );

    // Stealthed Rotation
    action_priority_list_t* stealthed = get_action_priority_list( "stealthed", "Stealthed Rotation" );
    stealthed->add_action( "shadowstrike,if=(buff.stealth.up|buff.vanish.up)&(spell_targets.shuriken_storm<4|variable.use_priority_rotation)&master_assassin_remains=0", "If Stealth/vanish are up, use Shadowstrike to benefit from the passive bonus and Find Weakness, even if we are at max CP (unless using Master Assassin)");
    stealthed->add_action( "gloomblade,if=combo_points=4&spell_targets.shuriken_storm<=4" );
    stealthed->add_action( "call_action_list,name=finish,if=variable.effective_combo_points>=cp_max_spend" );
    stealthed->add_action( "call_action_list,name=finish,if=buff.shuriken_tornado.up&combo_points.deficit<=2", "Finish at 3+ CP without DS / 4+ with DS with Shuriken Tornado buff up to avoid some CP waste situations." );
    stealthed->add_action( "call_action_list,name=finish,if=spell_targets.shuriken_storm>=4&variable.effective_combo_points>=4", "Also safe to finish at 4+ CP with exactly 4 targets. (Same as outside stealth.)" );
    stealthed->add_action( "call_action_list,name=finish,if=combo_points.deficit<=(talent.deeper_stratagem.enabled+talent.secret_stratagem.enabled+talent.seal_fate.enabled)" );
    stealthed->add_action( "shadowstrike,if=stealthed.sepsis&spell_targets.shuriken_storm<4" );
    stealthed->add_action( "backstab,if=conduit.perforated_veins.rank>=8&buff.perforated_veins.stack>=5&buff.shadow_dance.remains>=3&buff.shadow_blades.up&(spell_targets.shuriken_storm<=3|variable.use_priority_rotation)&(buff.shadow_blades.remains<=buff.shadow_dance.remains+2|!covenant.venthyr&!talent.flagellation)", "Backstab during Shadow Dance when on high PV stacks and Shadow Blades is up.");
    stealthed->add_action( "shiv,if=talent.nightstalker.enabled&runeforge.tiny_toxic_blade&spell_targets.shuriken_storm<5");
    stealthed->add_action( "shadowstrike,cycle_targets=1,if=!variable.use_priority_rotation&debuff.find_weakness.remains<1&spell_targets.shuriken_storm<=3&target.time_to_die-remains>6", "Up to 3 targets (no prio) keep up Find Weakness by cycling Shadowstrike." );
    stealthed->add_action( "shadowstrike,if=variable.use_priority_rotation&(debuff.find_weakness.remains<1|talent.weaponmaster.enabled&spell_targets.shuriken_storm<=4)", "For priority rotation, use Shadowstrike over Storm with WM against up to 4 targets or if FW is running off (on any amount of targets)" );
    stealthed->add_action( "shuriken_storm,if=spell_targets>=3+(buff.the_rotten.up|runeforge.akaaris_soul_fragment)&(!buff.premeditation.up|spell_targets>=5)");
    stealthed->add_action( "shadowstrike,if=debuff.find_weakness.remains<=1|cooldown.symbols_of_death.remains<18&debuff.find_weakness.remains<cooldown.symbols_of_death.remains", "Shadowstrike to refresh Find Weakness and to ensure we can carry over a full FW into the next SoD if possible." );
    stealthed->add_action( "gloomblade,if=buff.perforated_veins.stack>=5&conduit.perforated_veins.rank>=13" );
    stealthed->add_action( "shadowstrike" );
    stealthed->add_action( "cheap_shot,if=!target.is_boss&combo_points.deficit>=1&buff.shot_in_the_dark.up&energy.time_to_40>gcd.max");

    // Finishers
    action_priority_list_t* finish = get_action_priority_list( "finish", "Finishers" );
    finish->add_action( "variable,name=premed_snd_condition,value=talent.premeditation.enabled&spell_targets.shuriken_storm<(5-covenant.necrolord)&!covenant.kyrian", "While using Premeditation, avoid casting Slice and Dice when Shadow Dance is soon to be used, except for Kyrian" );
    finish->add_action( "slice_and_dice,if=!variable.premed_snd_condition&spell_targets.shuriken_storm<6&!buff.shadow_dance.up&buff.slice_and_dice.remains<fight_remains&refreshable");
    finish->add_action( "slice_and_dice,if=variable.premed_snd_condition&cooldown.shadow_dance.charges_fractional<1.75&buff.slice_and_dice.remains<cooldown.symbols_of_death.remains&(cooldown.shadow_dance.ready&buff.symbols_of_death.remains-buff.shadow_dance.remains<1.2)" );
    finish->add_action( "variable,name=skip_rupture,value=master_assassin_remains>0", "Helper Variable for Rupture. Skip during Master Assassin or during Dance with Dark and no Nightstalker." );
    finish->add_action( "rupture,if=(!variable.skip_rupture|variable.use_priority_rotation)&target.time_to_die-remains>6&refreshable", "Keep up Rupture if it is about to run out.");
    finish->add_action( "secret_technique");
    finish->add_action( "rupture,cycle_targets=1,if=!variable.skip_rupture&!variable.use_priority_rotation&spell_targets.shuriken_storm>=2&target.time_to_die>=(2*combo_points)&refreshable", "Multidotting targets that will live for the duration of Rupture, refresh during pandemic." );
    finish->add_action( "rupture,if=!variable.skip_rupture&remains<cooldown.symbols_of_death.remains+10&cooldown.symbols_of_death.remains<=5&target.time_to_die-remains>cooldown.symbols_of_death.remains+5", "Refresh Rupture early if it will expire during Symbols. Do that refresh if SoD gets ready in the next 5s." );
    finish->add_action( "black_powder,if=!variable.use_priority_rotation&spell_targets>=3");
    finish->add_action( "eviscerate" );

    // Builders
    action_priority_list_t* build = get_action_priority_list( "build", "Builders" );
    build->add_action( "shiv,if=!talent.nightstalker.enabled&runeforge.tiny_toxic_blade&spell_targets.shuriken_storm<5" );
    build->add_action( "shuriken_storm,if=spell_targets>=2&(!covenant.necrolord|cooldown.serrated_bone_spike.max_charges-charges_fractional>=0.25|spell_targets.shuriken_storm>4)&(buff.perforated_veins.stack<=4|spell_targets.shuriken_storm>4&!variable.use_priority_rotation)" );
    build->add_action( "serrated_bone_spike,if=buff.perforated_veins.stack<=2&(cooldown.serrated_bone_spike.max_charges-charges_fractional<=0.25|soulbind.lead_by_example.enabled&!buff.lead_by_example.up|soulbind.kevins_oozeling.enabled&!debuff.kevins_wrath.up)" );
    build->add_action( "gloomblade" );
    build->add_action( "backstab,if=!covenant.kyrian|!(variable.is_next_cp_animacharged&(time_to_sht.3.plus<0.5|time_to_sht.4.plus<1)&energy<60)", "Backstab immediately unless the next CP is Animacharged and we won't cap energy waiting for it.");
  }

  use_default_action_list = true;

  player_t::init_action_list();
}

// rogue_t::create_action  ==================================================

action_t* rogue_t::create_action( util::string_view name, util::string_view options_str )
{
  using namespace actions;

  if ( name == "adrenaline_rush"        ) return new adrenaline_rush_t        ( name, this, options_str );
  if ( name == "ambush"                 ) return new ambush_t                 ( name, this, options_str );
  if ( name == "apply_poison"           ) return new apply_poison_t           ( this, options_str );
  if ( name == "auto_attack"            ) return new auto_melee_attack_t      ( this, options_str );
  if ( name == "backstab"               ) return new backstab_t               ( name, this, options_str );
  if ( name == "between_the_eyes"       ) return new between_the_eyes_t       ( name, this, options_str );
  if ( name == "black_powder"           ) return new black_powder_t           ( name, this, options_str );
  if ( name == "blade_flurry"           ) return new blade_flurry_t           ( name, this, options_str );
  if ( name == "blade_rush"             ) return new blade_rush_t             ( name, this, options_str );
  if ( name == "cheap_shot"             ) return new cheap_shot_t             ( name, this, options_str );
  if ( name == "cold_blood"             ) return new cold_blood_t             ( name, this, options_str );
  if ( name == "crimson_tempest"        ) return new crimson_tempest_t        ( name, this, options_str );
  if ( name == "deathmark"              ) return new deathmark_t              ( name, this, options_str );
  if ( name == "detection"              ) return new detection_t              ( name, this, options_str );
  if ( name == "dispatch"               ) return new dispatch_t               ( name, this, options_str );
  if ( name == "dreadblades"            ) return new dreadblades_t            ( name, this, options_str );
  if ( name == "echoing_reprimand"      ) return new echoing_reprimand_t      ( name, this, options_str );
  if ( name == "envenom"                ) return new envenom_t                ( name, this, options_str );
  if ( name == "eviscerate"             ) return new eviscerate_t             ( name, this, options_str );
  if ( name == "exsanguinate"           ) return new exsanguinate_t           ( name, this, options_str );
  if ( name == "fan_of_knives"          ) return new fan_of_knives_t          ( name, this, options_str );
  if ( name == "feint"                  ) return new feint_t                  ( name, this, options_str );
  if ( name == "garrote"                ) return new garrote_t                ( name, this, spec.garrote, options_str );
  if ( name == "gouge"                  ) return new gouge_t                  ( name, this, options_str );
  if ( name == "ghostly_strike"         ) return new ghostly_strike_t         ( name, this, options_str );
  if ( name == "gloomblade"             ) return new gloomblade_t             ( name, this, options_str );
  if ( name == "indiscriminate_carnage" ) return new indiscriminate_carnage_t ( name, this, options_str );
  if ( name == "keep_it_rolling"        ) return new keep_it_rolling_t        ( name, this, options_str );
  if ( name == "kick"                   ) return new kick_t                   ( name, this, options_str );
  if ( name == "kidney_shot"            ) return new kidney_shot_t            ( name, this, options_str );
  if ( name == "killing_spree"          ) return new killing_spree_t          ( name, this, options_str );
  if ( name == "kingsbane"              ) return new kingsbane_t              ( name, this, options_str );
  if ( name == "marked_for_death"       ) return new marked_for_death_t       ( name, this, options_str );
  if ( name == "mutilate"               ) return new mutilate_t               ( name, this, options_str );
  if ( name == "pistol_shot"            ) return new pistol_shot_t            ( name, this, options_str );
  if ( name == "poisoned_knife"         ) return new poisoned_knife_t         ( name, this, options_str );
  if ( name == "roll_the_bones"         ) return new roll_the_bones_t         ( name, this, options_str );
  if ( name == "rupture"                ) return new rupture_t                ( name, this, spec.rupture, options_str );
  if ( name == "secret_technique"       ) return new secret_technique_t       ( name, this, options_str );
  if ( name == "shadow_blades"          ) return new shadow_blades_t          ( name, this, options_str );
  if ( name == "shadow_dance"           ) return new shadow_dance_t           ( name, this, options_str );
  if ( name == "shadowstep"             ) return new shadowstep_t             ( name, this, options_str );
  if ( name == "shadowstrike"           ) return new shadowstrike_t           ( name, this, options_str );
  if ( name == "shuriken_storm"         ) return new shuriken_storm_t         ( name, this, options_str );
  if ( name == "shuriken_tornado"       ) return new shuriken_tornado_t       ( name, this, options_str );
  if ( name == "shuriken_toss"          ) return new shuriken_toss_t          ( name, this, options_str );
  if ( name == "sinister_strike"        ) return new sinister_strike_t        ( name, this, options_str );
  if ( name == "slice_and_dice"         ) return new slice_and_dice_t         ( name, this, options_str );
  if ( name == "sprint"                 ) return new sprint_t                 ( name, this, options_str );
  if ( name == "stealth"                ) return new stealth_t                ( name, this, options_str );
  if ( name == "symbols_of_death"       ) return new symbols_of_death_t       ( name, this, options_str );
  if ( name == "shiv"                   ) return new shiv_t                   ( name, this, options_str );
  if ( name == "thistle_tea"            ) return new thistle_tea_t            ( name, this, options_str );
  if ( name == "vanish"                 ) return new vanish_t                 ( name, this, options_str );
  if ( name == "cancel_autoattack"      ) return new cancel_autoattack_t      ( this, options_str );
  if ( name == "swap_weapon"            ) return new weapon_swap_t            ( this, options_str );

  // Cross-Expansion Covenant Abilities

  if ( name == "flagellation" )
  {
    if ( talent.subtlety.flagellation->ok() )
      return new flagellation_t( name, this, options_str );
    else
      return new flagellation_covenant_t( name, this, options_str );
  }

  if ( name == "sepsis" )
  {
    if ( talent.shared.sepsis->ok() )
      return new sepsis_t( name, this, options_str );
    else
      return new sepsis_covenant_t( name, this, options_str );
  }

  if ( name == "serrated_bone_spike" )
  {
    if ( talent.assassination.serrated_bone_spike->ok() )
      return new serrated_bone_spike_t( name, this, options_str );
    else
      return new serrated_bone_spike_covenant_t( name, this, options_str );
  }

  return player_t::create_action( name, options_str );
}

// rogue_t::create_expression ===============================================

std::unique_ptr<expr_t> rogue_t::create_expression( util::string_view name_str )
{
  auto split = util::string_split<util::string_view>( name_str, "." );

  if ( split[0] == "combo_points" )
  {
    if ( split.size() == 1 )
    {
      return make_fn_expr( name_str, [ this ] { return this->current_cp( true ); } );
    }

    if(split.size() == 2 && split[1] == "deficit" )
    {
      return make_fn_expr( name_str, [ this ] { 
        return resources.max[ RESOURCE_COMBO_POINT ] - this->current_cp( true ); } );
    }
  }
  else if ( name_str == "effective_combo_points" )
  {
    return make_fn_expr( name_str, [ this ]() { return current_effective_cp( true, true ); } );
  }
  else if ( util::str_compare_ci( name_str, "cp_max_spend" ) )
  {
    return make_mem_fn_expr( name_str, *this, &rogue_t::consume_cp_max );
  }
  else if ( util::str_compare_ci( name_str, "master_assassin_remains" ) &&
            ( talent.assassination.master_assassin->ok() || !legendary.master_assassins_mark->ok() ) )
  {
    if ( !talent.assassination.master_assassin->ok() )
      return expr_t::create_constant( name_str, 0 );

    return make_fn_expr( name_str, [ this ]() {
      if ( buffs.master_assassin_aura->check() )
      {
        // Shadow Dance has no lingering effect
        if ( buffs.shadow_dance->check() )
          return buffs.shadow_dance->remains();

        timespan_t nominal_master_assassin_duration = timespan_t::from_seconds( talent.assassination.master_assassin->effectN( 1 ).base_value() );
        timespan_t gcd_remains = timespan_t::from_seconds( std::max( ( gcd_ready - sim->current_time() ).total_seconds(), 0.0 ) );
        return gcd_remains + nominal_master_assassin_duration;
      }
      return buffs.master_assassin->remains();
    } );
  }
  else if ( legendary.master_assassins_mark->ok() &&
            ( util::str_compare_ci( name_str, "master_assassins_mark_remains" ) ||
              util::str_compare_ci( name_str, "master_assassin_remains" ) ) )
  {
    return make_fn_expr( name_str, [ this ]() {
      if ( buffs.master_assassins_mark_aura->check() )
      {
        timespan_t nominal_master_assassin_duration = timespan_t::from_seconds( legendary.master_assassins_mark->effectN( 1 ).base_value() );
        timespan_t gcd_remains = timespan_t::from_seconds( std::max( ( gcd_ready - sim->current_time() ).total_seconds(), 0.0 ) );
        return gcd_remains + nominal_master_assassin_duration;
      }
      return buffs.master_assassins_mark->remains();
    } );
  }
  else if ( util::str_compare_ci( name_str, "improved_garrote_remains" ) )
  {
    if ( !talent.assassination.improved_garrote->ok() )
      return expr_t::create_constant( name_str, 0 );

    return make_fn_expr( name_str, [this]() {
      if ( buffs.improved_garrote_aura->check() )
      {
        // Shadow Dance has no lingering effect
        if ( buffs.shadow_dance->check() )
          return buffs.shadow_dance->remains();

        timespan_t nominal_duration = buffs.improved_garrote->base_buff_duration;
        timespan_t gcd_remains = timespan_t::from_seconds( std::max( ( gcd_ready - sim->current_time() ).total_seconds(), 0.0 ) );
        return gcd_remains + nominal_duration;
      }
      return buffs.improved_garrote->remains();
    } );
  }
  else if ( util::str_compare_ci( name_str, "poisoned" ) )
  {
    return make_fn_expr( name_str, [ this ]() {
      rogue_td_t* tdata = get_target_data( target );
      return tdata->is_lethal_poisoned();
    } );
  }
  else if ( util::str_compare_ci( name_str, "poison_remains" ) )
  {
    return make_fn_expr( name_str, [ this ]() {
      rogue_td_t* tdata = get_target_data( target );
      return tdata->lethal_poison_remains();
    } );
  }
  else if ( util::str_compare_ci( name_str, "bleeds" ) )
  {
    return make_fn_expr( name_str, [ this ]() {
      rogue_td_t* tdata = get_target_data( target );
      return tdata->dots.garrote->is_ticking() +
        tdata->dots.internal_bleeding->is_ticking() +
        tdata->dots.rupture->is_ticking() +
        tdata->dots.crimson_tempest->is_ticking() +
        tdata->dots.mutilated_flesh->is_ticking() +
        tdata->dots.serrated_bone_spike->is_ticking();
    } );
  }
  else if ( util::str_compare_ci( name_str, "poisoned_bleeds" ) )
  {
    return make_fn_expr( name_str, [ this ]() {
      int poisoned_bleeds = 0;
      for ( auto p : sim->target_non_sleeping_list )
      {
        rogue_td_t* tdata = get_target_data( p );
        if ( tdata->is_lethal_poisoned() )
        {
          poisoned_bleeds += tdata->dots.garrote->is_ticking() +
            tdata->dots.internal_bleeding->is_ticking() +
            tdata->dots.rupture->is_ticking();
        }
      }
      return poisoned_bleeds;
    } );
  }
  else if ( util::str_compare_ci( name_str, "active_bone_spikes" ) )
  {
    if ( !covenant.serrated_bone_spike->ok() && !talent.assassination.serrated_bone_spike->ok() )
    {
      return expr_t::create_constant( name_str, 0 );
    }

    action_t* action = nullptr;
    if ( covenant.serrated_bone_spike->ok() )
      action = find_background_action<actions::serrated_bone_spike_covenant_t::serrated_bone_spike_dot_t>( "serrated_bone_spike_dot" );
    else
      action = find_background_action<actions::serrated_bone_spike_t::serrated_bone_spike_dot_t>( "serrated_bone_spike_dot" );

    if ( !action )
    {
      return expr_t::create_constant( name_str, 0 );
    }

    return make_fn_expr( name_str, [ this, action ]() {
      return get_active_dots( action->internal_id );
    } );
  }
  else if ( util::str_compare_ci( name_str, "rtb_buffs" ) )
  {
    if ( specialization() != ROGUE_OUTLAW )
    {
      return expr_t::create_constant( name_str, 0 );
    }

    return make_fn_expr( name_str, [ this ]() {
      double n_buffs = 0;
      n_buffs += buffs.skull_and_crossbones->check() != 0;
      n_buffs += buffs.grand_melee->check() != 0;
      n_buffs += buffs.ruthless_precision->check() != 0;
      n_buffs += buffs.true_bearing->check() != 0;
      n_buffs += buffs.broadside->check() != 0;
      n_buffs += buffs.buried_treasure->check() != 0;
      return n_buffs;
    } );
  }
  else if ( util::str_compare_ci(name_str, "priority_rotation") )
  {
    return expr_t::create_constant( name_str, options.priority_rotation );
  }
  else if ( util::str_compare_ci( name_str, "prepull_shadowdust" ) )
  {
    return expr_t::create_constant( name_str, options.prepull_shadowdust );
  }

  // Split expressions

  // stealthed.(rogue|mantle|basic|sepsis|all)
  // rogue: all rogue abilities are checked (stealth, vanish, shadow_dance, subterfuge)
  // mantle: all abilities that maintain Mantle of the Master Assassin aura are checked (stealth, vanish)
  // all: all abilities that allow stealth are checked (rogue + shadowmeld)
  if ( split.size() == 2 && util::str_compare_ci( split[ 0 ], "stealthed" ) )
  {
    if ( util::str_compare_ci( split[ 1 ], "rogue" ) )
    {
      return make_fn_expr( split[ 0 ], [ this ]() {
        return stealthed( STEALTH_BASIC | STEALTH_ROGUE );
      } );
    }
    else if ( util::str_compare_ci( split[ 1 ], "mantle" ) || util::str_compare_ci( split[ 1 ], "basic" ) )
    {
      return make_fn_expr( split[ 0 ], [ this ]() {
        return stealthed( STEALTH_BASIC );
      } );
    }
    else if ( util::str_compare_ci( split[ 1 ], "sepsis" ) )
    {
      return make_fn_expr( split[ 0 ], [ this ]() {
        return stealthed( STEALTH_SEPSIS );
      } );
    }
    else if ( util::str_compare_ci( split[ 1 ], "improved_garrote" ) )
    {
      if ( !talent.assassination.improved_garrote->ok() )
        return expr_t::create_constant( name_str, false );

      return make_fn_expr( split[ 0 ], [this]() {
        return stealthed( STEALTH_IMPROVED_GARROTE );
      } );
    }
    else if ( util::str_compare_ci( split[ 1 ], "all" ) )
    {
      return make_fn_expr( split[ 0 ], [ this ]() {
        return stealthed();
      } );
    }
  }
  // exsanguinated.(garrote|internal_bleeding|rupture|crimson_tempest)
  else if ( split.size() == 2 && util::str_compare_ci( split[ 0 ], "exsanguinated" ) &&
    ( util::str_compare_ci( split[ 1 ], "garrote" ) ||
      util::str_compare_ci( split[ 1 ], "internal_bleeding" ) ||
      util::str_compare_ci( split[ 1 ], "rupture" ) ||
      util::str_compare_ci( split[ 1 ], "crimson_tempest" ) ||
      util::str_compare_ci( split[ 1 ], "serrated_bone_spike" ) ) )
  {
    action_t* action = find_action( split[ 1 ] );
    if ( !action )
    {
      return expr_t::create_constant( "exsanguinated_expr", 0 );
    }

    return make_fn_expr( name_str, [ action ]() {
      dot_t* d = action->get_dot( action->target );
      return d->is_ticking() && actions::rogue_attack_t::cast_state( d->state )->is_exsanguinated();
    } );
  }
  // rtb_list.<buffs>
  else if ( split.size() == 3 && util::str_compare_ci( split[ 0 ], "rtb_list" ) && ! split[ 1 ].empty() )
  {
    enum class rtb_list_type
    {
      NONE = 0U,
      ANY,
      ALL
    };

    const std::array<const buff_t*, 6> rtb_buffs = { {
      buffs.broadside,
      buffs.buried_treasure,
      buffs.grand_melee,
      buffs.ruthless_precision,
      buffs.skull_and_crossbones,
      buffs.true_bearing
    } };

    // Figure out the type
    rtb_list_type type = rtb_list_type::NONE;
    if ( util::str_compare_ci( split[ 1 ], "any" ) )
    {
      type = rtb_list_type::ANY;
    }
    else if ( util::str_compare_ci( split[ 1 ], "all" ) )
    {
      type = rtb_list_type::ALL;
    }

    // Parse the buff numeric list to values that index rtb_buffs above
    std::vector<unsigned> list_values;
    range::for_each( split[ 2 ], [ &list_values ]( char v ) {
      if ( v < '1' || v > '6' )
      {
        return;
      }

      list_values.push_back( v - '1' );
    } );

    // If we have buffs and an operating mode, make an expression
    if ( type != rtb_list_type::NONE && !list_values.empty() )
    {
      return make_fn_expr( split[ 0 ], [ type, rtb_buffs, list_values ]() {
        for ( unsigned int list_value : list_values )
        {
          if ( type == rtb_list_type::ANY && rtb_buffs[ list_value ]->check() )
          {
            return 1;
          }
          else if ( type == rtb_list_type::ALL && !rtb_buffs[ list_value ]->check() )
          {
            return 0;
          }
        }

        return type == rtb_list_type::ANY ? 0 : 1;
      } );
    }
  }
  // time_to_sht.(1|2|3|4|5)
  // time_to_sht.(1|2|3|4|5).plus
  // x: returns time until we will do the xth attack since last ShT proc.
  // plus: denotes to return the timer for the next swing if we are past that counter
  if ( split[ 0 ] == "time_to_sht" )
  {
    unsigned attack_x = split.size() > 1 ? util::to_unsigned( split[ 1 ] ) : 4;
    bool plus = split.size() > 2 ? split[ 2 ] == "plus" : false;
    
    return make_fn_expr( split[ 0 ], [ this, attack_x, plus ]() {
      timespan_t return_value = timespan_t::from_seconds( 0.0 );

      // If we are testing against a high-probability attack count, and we are still reacting, use the reaction time
      if ( attack_x >= 4 && buffs.shadow_techniques->check() && !buffs.shadow_techniques->stack_react() )
      {
        return_value = buffs.shadow_techniques->stack_react_time[ 1 ] - sim->current_time();
        sim->print_debug( "{} time_to_sht: proc recently occurred and we are still reacting", *this );
      }
      else if ( main_hand_attack && ( attack_x > shadow_techniques_counter || plus ) && attack_x <= 5 )
      {
        unsigned remaining_aa;
        if ( attack_x <= shadow_techniques_counter && plus )
        {
          remaining_aa = 1;
          sim->print_debug( "{} time_to_sht: attack_x = {}+, count at {}, returning next", *this, attack_x, shadow_techniques_counter );
        }
        else
        {
          remaining_aa = attack_x - shadow_techniques_counter;
          sim->print_debug( "{} time_to_sht: attack_x = {}, remaining_aa = {}", *this, attack_x, remaining_aa );
        }
        
        timespan_t mh_swing_time = main_hand_attack->execute_time();
        timespan_t mh_next_swing = timespan_t::from_seconds( 0.0 );
        if ( main_hand_attack->execute_event == nullptr )
        {
          mh_next_swing = mh_swing_time;
        }
        else
        {
          mh_next_swing = main_hand_attack->execute_event->remains();
        }
        sim->print_debug( "{} time_to_sht: MH swing_time: {}, next_swing in: {}", *this, mh_swing_time, mh_next_swing );

        timespan_t oh_swing_time = timespan_t::from_seconds( 0.0 );
        timespan_t oh_next_swing = timespan_t::from_seconds( 0.0 );
        if ( off_hand_attack )
        {
          oh_swing_time = off_hand_attack->execute_time();
          if ( off_hand_attack->execute_event == nullptr )
          {
            oh_next_swing = oh_swing_time;
          }
          else
          {
            oh_next_swing = off_hand_attack->execute_event->remains();
          }
          sim->print_debug( "{} time_to_sht: OH swing_time: {}, next_swing in: {}", *this, oh_swing_time, oh_next_swing );
        }
        else
        {
          sim->print_debug( "{} time_to_sht: OH attack not found, using only MH timers", *this );
        }

        // Store upcoming attack timers and sort
        std::vector<timespan_t> attacks;
        attacks.reserve( 2 * remaining_aa );
        for ( size_t i = 0; i < remaining_aa; i++ )
        {
          attacks.push_back( mh_next_swing + i * mh_swing_time );
          if ( off_hand_attack )
            attacks.push_back( oh_next_swing + i * oh_swing_time );
        }
        range::sort( attacks );

        // Add player reaction time to the predicted value as players still need to react to the swing and proc
        timespan_t total_reaction_time = this->total_reaction_time();
        return_value = attacks.at( remaining_aa - 1 ) + total_reaction_time;
      }
      else if ( main_hand_attack == nullptr )
      {
        return_value = timespan_t::from_seconds( 0.0 );
        sim->print_debug( "{} time_to_sht: MH attack is required but was not found", *this );
      }
      else if ( attack_x > 5 )
      {
        return_value = timespan_t::from_seconds( 0.0 );
        sim->print_debug( "{} time_to_sht: Invalid value {} for attack_x (must be 5 or less)", *this, attack_x );
      }
      else
      {
        return_value = timespan_t::from_seconds( 0.0 );
        sim->print_debug( "{} time_to_sht: attack_x value {} is not greater than shadow techniques count {}", *this, attack_x, shadow_techniques_counter );
      }
      sim->print_debug( "{} time_to_sht: return value is: {}", *this, return_value );
      return return_value;
    } );
  }

  return player_t::create_expression( name_str );
}

std::unique_ptr<expr_t> rogue_t::create_resource_expression( util::string_view name_str )
{
  auto splits = util::string_split<util::string_view>( name_str, "." );
  if ( splits.empty() )
    return nullptr;

  resource_e r = util::parse_resource_type( splits[ 0 ] );
  if ( r == RESOURCE_ENERGY )
  {
    // Custom Rogue Energy Deficit
    // Ignores temporary energy max when calculating the current deficit
    if ( splits.size() == 2 && ( splits[ 1 ] == "base_deficit" ) )
    {
      return make_fn_expr( name_str, [this] {
        return std::max( resources.max[ RESOURCE_ENERGY ] -
                         resources.current[ RESOURCE_ENERGY ] -
                         resources.temporary[ RESOURCE_ENERGY ], 0.0 );
      } );
    }
    // Custom Rogue Energy Regen Functions
    // Optionally handles things that are outside of the normal resource_regen_per_second flow
    if ( splits.size() == 2 && ( splits[ 1 ] == "regen" || splits[ 1 ] == "regen_combined" ||
                                 splits[ 1 ] == "time_to_max" || splits[ 1 ] == "time_to_max_combined" || 
                                 splits[ 1 ] == "base_time_to_max" || splits[ 1 ] == "base_time_to_max_combined" ) )
    {
      const bool regen = util::str_prefix_ci( splits[ 1 ], "regen" );
      const bool combined = util::str_in_str_ci( splits[ 1 ], "_combined" );
      const bool base_max = util::str_prefix_ci( splits[ 1 ], "base" );

      return make_fn_expr( name_str, [ this, regen, combined, base_max] {
        double energy_deficit = resources.max[ RESOURCE_ENERGY ] - resources.current[ RESOURCE_ENERGY ];
        double energy_regen_per_second = resource_regen_per_second( RESOURCE_ENERGY );

        if ( base_max )
        {
          energy_deficit = std::max( energy_deficit - resources.temporary[ RESOURCE_ENERGY ], 0.0 );
        }

        if ( buffs.adrenaline_rush->check() )
        {
          energy_regen_per_second *= 1.0 + buffs.adrenaline_rush->data().effectN( 1 ).percent();
        }

        energy_regen_per_second += buffs.buried_treasure->check_value();

        if ( buffs.blade_rush->check() )
        {
          energy_regen_per_second += ( buffs.blade_rush->data().effectN( 1 ).base_value() / buffs.blade_rush->buff_period.total_seconds() );
        }

        // Combined non-traditional or inconsistent regen sources
        if ( combined )
        {
          if ( specialization() == ROGUE_ASSASSINATION )
          {
            double poisoned_bleeds = 0;
            int lethal_poisons = 0;
            for ( auto p : sim->target_non_sleeping_list )
            {
              rogue_td_t* tdata = get_target_data( p );
              if ( tdata->is_lethal_poisoned() )
              {
                lethal_poisons++;
                auto bleeds = { tdata->dots.garrote, tdata->dots.internal_bleeding, tdata->dots.rupture };
                for ( auto bleed : bleeds )
                {
                  if ( bleed && bleed->is_ticking() )
                  {
                    // Multiply Venomous Wounds contribution by the Exsanguinated rate multiplier
                    poisoned_bleeds += ( 1.0 * cast_attack( bleed->current_action )->cast_state( bleed->state )->get_exsanguinated_rate() );
                  }
                }
              }
            }

            // Venomous Wounds
            const double dot_tick_rate = 2.0 * composite_spell_haste();
            energy_regen_per_second += ( poisoned_bleeds * talent.assassination.venomous_wounds->effectN( 2 ).base_value() ) / dot_tick_rate;

            // Dashing Scoundrel -- Estimate ~90% Envenom uptime for energy regen approximation
            if ( spec.dashing_scoundrel_gain > 0.0 )
            {
              energy_regen_per_second += ( 0.9 * lethal_poisons * active.lethal_poison->composite_crit_chance() * spec.dashing_scoundrel_gain ) / dot_tick_rate;
            }
          }
        }

        // TODO - Add support for Master of Shadows, and potentially also estimated Combat Potency, ShT etc.
        //        Also consider if buffs such as Adrenaline Rush should be prorated based on duration for time_to_max

        return regen ? energy_regen_per_second : energy_deficit / energy_regen_per_second;
      } );
    }
  }

  return player_t::create_resource_expression( name_str );
}

// rogue_t::init_base =======================================================

void rogue_t::init_base_stats()
{
  if ( base.distance < 1 )
    base.distance = 5;

  player_t::init_base_stats();

  base.attack_power_per_strength = 0.0;
  base.attack_power_per_agility  = 1.0;
  base.spell_power_per_intellect = 1.0;

  resources.base[ RESOURCE_COMBO_POINT ] = 5;
  resources.base[ RESOURCE_COMBO_POINT ] += talent.rogue.deeper_stratagem->effectN( 2 ).base_value();
  resources.base[ RESOURCE_COMBO_POINT ] += talent.outlaw.devious_stratagem->effectN( 2 ).base_value();
  resources.base[ RESOURCE_COMBO_POINT ] += talent.subtlety.secret_stratagem->effectN( 2 ).base_value();

  resources.base[ RESOURCE_ENERGY ] = 100;
  resources.base[ RESOURCE_ENERGY ] += talent.rogue.vigor->effectN( 1 ).base_value();
  resources.base[ RESOURCE_ENERGY ] += spec.assassination_rogue->effectN( 5 ).base_value();

  resources.base_regen_per_second[ RESOURCE_ENERGY ] = 10;
  resources.base_regen_per_second[ RESOURCE_ENERGY ] *= 1.0 + talent.outlaw.combat_potency->effectN( 1 ).percent();
  resources.base_regen_per_second[ RESOURCE_ENERGY ] *= 1.0 + talent.rogue.vigor->effectN( 2 ).percent();

  base_gcd = timespan_t::from_seconds( 1.0 );
  min_gcd  = timespan_t::from_seconds( 1.0 );

  if ( options.rogue_ready_trigger )
  {
    ready_type = READY_TRIGGER;
  }
}

// rogue_t::init_spells =====================================================

void rogue_t::init_spells()
{
  player_t::init_spells();

  // Core Class Spells
  spell.ambush = find_class_spell( "Ambush" );
  spell.cheap_shot = find_class_spell( "Cheap Shot" );
  spell.crimson_vial = find_class_spell( "Crimson Vial" );
  spell.crippling_poison = find_class_spell( "Crippling Poison" );
  spell.detection = find_class_spell( "Detection" );
  spell.instant_poison = find_class_spell( "Instant Poison" );
  spell.kick = find_class_spell( "Kick" );
  spell.kidney_shot = find_class_spell( "Kidney Shot" );
  spell.slice_and_dice = find_class_spell( "Slice and Dice" );
  spell.sprint = find_class_spell( "Sprint" );
  spell.stealth = find_class_spell( "Stealth" );
  spell.vanish = find_class_spell( "Vanish" );
  spell.wound_poison = find_class_spell( "Wound Poison" );

  // Class Passives
  spell.all_rogue = find_spell( 137034 );
  spell.critical_strikes = find_class_spell( "Critical Strikes" );
  spell.fleet_footed = find_class_spell( "Fleet Footed" );

  // Assassination Spells
  spec.assassination_rogue = find_specialization_spell( "Assassination Rogue" );
  spec.envenom = find_specialization_spell( "Envenom" );
  spec.fan_of_knives = find_specialization_spell( "Fan of Knives" );
  spec.garrote = find_specialization_spell( "Garrote" );
  spec.mutilate = find_specialization_spell( "Mutilate" );
  spec.poisoned_knife = find_specialization_spell( "Poisoned Knife" );

  // Outlaw Spells
  spec.outlaw_rogue = find_specialization_spell( "Outlaw Rogue" );
  spec.between_the_eyes = find_specialization_spell( "Between the Eyes" );
  spec.dispatch = find_specialization_spell( "Dispatch" );
  spec.pistol_shot = find_specialization_spell( "Pistol Shot" );
  spec.sinister_strike = find_specialization_spell( "Sinister Strike" );

  // Subtlety Spells
  spec.subtlety_rogue = find_specialization_spell( "Subtlety Rogue" );
  spec.backstab = find_specialization_spell( "Backstab" );
  spec.eviscerate = find_class_spell( "Eviscerate" ); // Baseline spell for early leveling
  spec.shadow_dance = find_specialization_spell( "Shadow Dance" );
  spec.shadow_techniques = find_specialization_spell( "Shadow Techniques" );
  spec.shadowstrike = find_specialization_spell( "Shadowstrike" );
  spec.shuriken_toss = find_specialization_spell( "Shuriken Toss" );
  spec.shuriken_storm = find_specialization_spell( "Shuriken Storm" );
  spec.symbols_of_death = find_specialization_spell( "Symbols of Death" );

  // Multi-Spec
  spec.rupture = find_specialization_spell( "Rupture" ); // Assassination + Subtlety

  // Masteries
  mastery.potent_assassin = find_mastery_spell( ROGUE_ASSASSINATION );
  mastery.main_gauche = find_mastery_spell( ROGUE_OUTLAW );
  mastery.main_gauche_attack = mastery.main_gauche->ok() ? find_spell( 86392 ) : spell_data_t::not_found();
  mastery.executioner = find_mastery_spell( ROGUE_SUBTLETY );

  // Class Talents
  talent.rogue.shiv = find_talent_spell( talent_tree::CLASS, "Shiv" );
  talent.rogue.blind = find_talent_spell( talent_tree::CLASS, "Blind" );
  talent.rogue.sap = find_talent_spell( talent_tree::CLASS, "Sap" );

  talent.rogue.evasion = find_talent_spell( talent_tree::CLASS, "Evasion" );
  talent.rogue.feint = find_talent_spell( talent_tree::CLASS, "Feint" );
  talent.rogue.cloak_of_shadows = find_talent_spell( talent_tree::CLASS, "Cloak of Shadows" );

  talent.rogue.master_poisoner = find_talent_spell( talent_tree::CLASS, "Master Poisoner" );
  talent.rogue.numbing_poison = find_talent_spell( talent_tree::CLASS, "Numbing Poison" );
  talent.rogue.atrophic_poison = find_talent_spell( talent_tree::CLASS, "Atrophic Poison" );
  talent.rogue.nimble_fingers = find_talent_spell( talent_tree::CLASS, "Nimble Fingers" );
  talent.rogue.gouge = find_talent_spell( talent_tree::CLASS, "Gouge" );
  talent.rogue.rushed_setup = find_talent_spell( talent_tree::CLASS, "Rushed Setup" );
  talent.rogue.tricks_of_the_trade = find_talent_spell( talent_tree::CLASS, "Tricks of the Trade" );
  talent.rogue.shadowrunner = find_talent_spell( talent_tree::CLASS, "Shadowrunner" );

  talent.rogue.improved_wound_poison = find_talent_spell( talent_tree::CLASS, "Improved Wound Poison" );
  talent.rogue.fleet_footed = find_talent_spell( talent_tree::CLASS, "Fleet Footed" );
  talent.rogue.iron_stomach = find_talent_spell( talent_tree::CLASS, "Iron Stomach" );
  talent.rogue.improved_sprint = find_talent_spell( talent_tree::CLASS, "Improved Sprint" );
  talent.rogue.prey_on_the_weak = find_talent_spell( talent_tree::CLASS, "Prey on the Weak" );
  talent.rogue.shadowstep = find_talent_spell( talent_tree::CLASS, "Shadowstep" );
  talent.rogue.subterfuge = find_talent_spell( talent_tree::CLASS, "Subterfuge" );

  talent.rogue.deadened_nerves = find_talent_spell( talent_tree::CLASS, "Deadened Nerves" );
  talent.rogue.cheat_death = find_talent_spell( talent_tree::CLASS, "Cheat Death" );
  talent.rogue.elusiveness = find_talent_spell( talent_tree::CLASS, "Elusiveness" );
  talent.rogue.blackjack = find_talent_spell( talent_tree::CLASS, "Blackjack" );

  talent.rogue.deadly_precision = find_talent_spell( talent_tree::CLASS, "Deadly Precision" );
  talent.rogue.virulent_poisons = find_talent_spell( talent_tree::CLASS, "Virulent Poisons" );
  talent.rogue.thiefs_versatility = find_talent_spell( talent_tree::CLASS, "Thief's Versatility" );
  talent.rogue.tight_spender = find_talent_spell( talent_tree::CLASS, "Tight Spender" );
  talent.rogue.nightstalker = find_talent_spell( talent_tree::CLASS, "Nightstalker" );

  talent.rogue.vigor = find_talent_spell( talent_tree::CLASS, "Vigor" );
  talent.rogue.acrobatic_strikes = find_talent_spell( talent_tree::CLASS, "Acrobatic Strikes" );
  talent.rogue.improved_ambush = find_talent_spell( talent_tree::CLASS, "Improved Ambush" );

  talent.rogue.leeching_poison = find_talent_spell( talent_tree::CLASS, "Leeching Poison" );
  talent.rogue.lethality = find_talent_spell( talent_tree::CLASS, "Lethality" );
  talent.rogue.recuperator = find_talent_spell( talent_tree::CLASS, "Recuperator" );
  talent.rogue.alacrity = find_talent_spell( talent_tree::CLASS, "Alacrity" );
  talent.rogue.soothing_darkness = find_talent_spell( talent_tree::CLASS, "Soothing Darkness" );

  talent.rogue.seal_fate = find_talent_spell( talent_tree::CLASS, "Seal Fate" );
  talent.rogue.cold_blood = find_talent_spell( talent_tree::CLASS, "Cold Blood" );
  talent.rogue.echoing_reprimand = find_talent_spell( talent_tree::CLASS, "Echoing Reprimand" );
  talent.rogue.deeper_stratagem = find_talent_spell( talent_tree::CLASS, "Deeper Stratagem" );
  talent.rogue.marked_for_death = find_talent_spell( talent_tree::CLASS, "Marked for Death" );
  talent.rogue.find_weakness = find_talent_spell( talent_tree::CLASS, "Find Weakness" );

  talent.rogue.thistle_tea = find_talent_spell( talent_tree::CLASS, "Thistle Tea" );
  talent.rogue.resounding_clarity = find_talent_spell( talent_tree::CLASS, "Resounding Clarity" );
  talent.rogue.reverberation = find_talent_spell( talent_tree::CLASS, "Reverberation" );
  talent.rogue.shadow_dance = find_talent_spell( talent_tree::CLASS, "Shadow Dance" );

  // Assassination Talents
  talent.assassination.deadly_poison = find_talent_spell( talent_tree::SPECIALIZATION, "Deadly Poison" );

  talent.assassination.improved_shiv = find_talent_spell( talent_tree::SPECIALIZATION, "Improved Shiv" );
  talent.assassination.venomous_wounds = find_talent_spell( talent_tree::SPECIALIZATION, "Venomous Wounds" );
  talent.assassination.shadowstep = find_talent_spell( talent_tree::SPECIALIZATION, "Shadowstep", ROGUE_ASSASSINATION );

  talent.assassination.cut_to_the_chase = find_talent_spell( talent_tree::SPECIALIZATION, "Cut to the Chase" );
  talent.assassination.elaborate_planning = find_talent_spell( talent_tree::SPECIALIZATION, "Elaborate Planning" );
  talent.assassination.improved_poisons = find_talent_spell( talent_tree::SPECIALIZATION, "Improved Poisons" );
  talent.assassination.bloody_mess = find_talent_spell( talent_tree::SPECIALIZATION, "Bloody Mess" );
  talent.assassination.internal_bleeding = find_talent_spell( talent_tree::SPECIALIZATION, "Internal Bleeding" );

  talent.assassination.thrown_precision = find_talent_spell( talent_tree::SPECIALIZATION, "Thrown Precision" );
  talent.assassination.lightweight_shiv = find_talent_spell( talent_tree::SPECIALIZATION, "Lightweight Shiv" );
  talent.assassination.fatal_concoction = find_talent_spell( talent_tree::SPECIALIZATION, "Fatal Concoction" );
  talent.assassination.improved_garrote = find_talent_spell( talent_tree::SPECIALIZATION, "Improved Garrote" );
  talent.assassination.intent_to_kill = find_talent_spell( talent_tree::SPECIALIZATION, "Intent to Kill" );

  talent.assassination.crimson_tempest = find_talent_spell( talent_tree::SPECIALIZATION, "Crimson Tempest" );
  talent.assassination.venom_rush = find_talent_spell( talent_tree::SPECIALIZATION, "Venom Rush" );
  talent.assassination.deathmark = find_talent_spell( talent_tree::SPECIALIZATION, "Deathmark" );
  talent.assassination.master_assassin = find_talent_spell( talent_tree::SPECIALIZATION, "Master Assassin" );
  talent.assassination.exsanguinate = find_talent_spell( talent_tree::SPECIALIZATION, "Exsanguinate" );

  talent.assassination.flying_daggers = find_talent_spell( talent_tree::SPECIALIZATION, "Flying Daggers" );
  talent.assassination.vicious_venoms = find_talent_spell( talent_tree::SPECIALIZATION, "Vicious Venoms" );
  talent.assassination.lethal_dose = find_talent_spell( talent_tree::SPECIALIZATION, "Lethal Dose" );
  talent.assassination.iron_wire = find_talent_spell( talent_tree::SPECIALIZATION, "Iron Wire" );

  talent.assassination.systemic_failure = find_talent_spell( talent_tree::SPECIALIZATION, "Systemic Failure" );
  talent.assassination.amplifying_poison = find_talent_spell( talent_tree::SPECIALIZATION, "Amplifying Poison" );
  talent.assassination.twist_the_knife = find_talent_spell( talent_tree::SPECIALIZATION, "Twist the Knife" );
  talent.assassination.doomblade = find_talent_spell( talent_tree::SPECIALIZATION, "Doomblade" );

  talent.assassination.blindside = find_talent_spell( talent_tree::SPECIALIZATION, "Blindside" );
  talent.assassination.tiny_toxic_blade = find_talent_spell( talent_tree::SPECIALIZATION, "Tiny Toxic Blade" );
  talent.assassination.poison_bomb = find_talent_spell( talent_tree::SPECIALIZATION, "Poison Bomb" );
  talent.assassination.shrouded_suffocation = find_talent_spell( talent_tree::SPECIALIZATION, "Shrouded Suffocation" );
  talent.assassination.sepsis = find_talent_spell( talent_tree::SPECIALIZATION, "Sepsis", ROGUE_ASSASSINATION );
  talent.assassination.serrated_bone_spike = find_talent_spell( talent_tree::SPECIALIZATION, "Serrated Bone Spike" );

  talent.assassination.zoldyck_recipe = find_talent_spell( talent_tree::SPECIALIZATION, "Zoldyck Recipe" );
  talent.assassination.dashing_scoundrel = find_talent_spell( talent_tree::SPECIALIZATION, "Dashing Scoundrel" );
  talent.assassination.scent_of_blood = find_talent_spell( talent_tree::SPECIALIZATION, "Scent of Blood" );

  talent.assassination.kingsbane = find_talent_spell( talent_tree::SPECIALIZATION, "Kingsbane" );
  talent.assassination.dragon_tempered_blades = find_talent_spell( talent_tree::SPECIALIZATION, "Dragon-Tempered Blades" );
  talent.assassination.indiscriminate_carnage = find_talent_spell( talent_tree::SPECIALIZATION, "Indiscriminate Carnage" );

  // Outlaw Talents
  talent.outlaw.opportunity = find_talent_spell( talent_tree::SPECIALIZATION, "Opportunity" );
  talent.outlaw.blade_flurry = find_talent_spell( talent_tree::SPECIALIZATION, "Blade Flurry" );

  talent.outlaw.grappling_hook = find_talent_spell( talent_tree::SPECIALIZATION, "Grappling Hook" );
  talent.outlaw.weaponmaster = find_talent_spell( talent_tree::SPECIALIZATION, "Weaponmaster", ROGUE_OUTLAW );
  talent.outlaw.combat_potency = find_talent_spell( talent_tree::SPECIALIZATION, "Combat Potency" );
  talent.outlaw.ambidexterity = find_talent_spell( talent_tree::SPECIALIZATION, "Ambidexterity" );
  talent.outlaw.hit_and_run = find_talent_spell( talent_tree::SPECIALIZATION, "Hit and Run" );

  talent.outlaw.retractable_hook = find_talent_spell( talent_tree::SPECIALIZATION, "Retractable Hook" );
  talent.outlaw.combat_stamina = find_talent_spell( talent_tree::SPECIALIZATION, "Combat Stamina" );
  talent.outlaw.adrenaline_rush = find_talent_spell( talent_tree::SPECIALIZATION, "Adrenaline Rush" );
  talent.outlaw.riposte = find_talent_spell( talent_tree::SPECIALIZATION, "Riposte" );
  talent.outlaw.deft_maneuvers = find_talent_spell( talent_tree::SPECIALIZATION, "Deft Maneuvers" );

  talent.outlaw.blinding_powder = find_talent_spell( talent_tree::SPECIALIZATION, "Blinding Powder" );
  talent.outlaw.ruthlessness = find_talent_spell( talent_tree::SPECIALIZATION, "Ruthlessness" );
  talent.outlaw.swift_slasher = find_talent_spell( talent_tree::SPECIALIZATION, "Swift Slasher" );
  talent.outlaw.restless_blades = find_talent_spell( talent_tree::SPECIALIZATION, "Restless Blades" );
  talent.outlaw.fatal_flourish = find_talent_spell( talent_tree::SPECIALIZATION, "Fatal Flourish" );
  talent.outlaw.improved_between_the_eyes = find_talent_spell( talent_tree::SPECIALIZATION, "Improved Between the Eyes" );
  talent.outlaw.dirty_tricks = find_talent_spell( talent_tree::SPECIALIZATION, "Dirty Tricks" );

  talent.outlaw.heavy_hitter = find_talent_spell( talent_tree::SPECIALIZATION, "Heavy Hitter" );
  talent.outlaw.devious_stratagem = find_talent_spell( talent_tree::SPECIALIZATION, "Devious Stratagem" );
  talent.outlaw.roll_the_bones = find_talent_spell( talent_tree::SPECIALIZATION, "Roll the Bones" );
  talent.outlaw.quick_draw = find_talent_spell( talent_tree::SPECIALIZATION, "Quick Draw" );
  talent.outlaw.ace_up_your_sleeve = find_talent_spell( talent_tree::SPECIALIZATION, "Ace Up Your Sleeve" );

  talent.outlaw.audacity = find_talent_spell( talent_tree::SPECIALIZATION, "Audacity" );
  talent.outlaw.loaded_dice = find_talent_spell( talent_tree::SPECIALIZATION, "Loaded Dice" );
  talent.outlaw.float_like_a_butterfly = find_talent_spell( talent_tree::SPECIALIZATION, "Float Like a Butterfly" );
  talent.outlaw.sleight_of_hand = find_talent_spell( talent_tree::SPECIALIZATION, "Sleight of Hand" );
  talent.outlaw.dancing_steel = find_talent_spell( talent_tree::SPECIALIZATION, "Dancing Steel" );

  talent.outlaw.triple_threat = find_talent_spell( talent_tree::SPECIALIZATION, "Triple Threat" );
  talent.outlaw.count_the_odds = find_talent_spell( talent_tree::SPECIALIZATION, "Count the Odds" );
  talent.outlaw.improved_main_gauche = find_talent_spell( talent_tree::SPECIALIZATION, "Improved Main Gauche" );

  talent.outlaw.sepsis = find_talent_spell( talent_tree::SPECIALIZATION, "Sepsis", ROGUE_OUTLAW );
  talent.outlaw.ghostly_strike = find_talent_spell( talent_tree::SPECIALIZATION, "Ghostly Strike" );
  talent.outlaw.blade_rush = find_talent_spell( talent_tree::SPECIALIZATION, "Blade Rush" );
  talent.outlaw.improved_adrenaline_rush = find_talent_spell( talent_tree::SPECIALIZATION, "Improved Adrenaline Rush" );
  talent.outlaw.killing_spree = find_talent_spell( talent_tree::SPECIALIZATION, "Killing Spree" );
  talent.outlaw.dreadblades = find_talent_spell( talent_tree::SPECIALIZATION, "Dreadblades" );
  talent.outlaw.precise_cuts = find_talent_spell( talent_tree::SPECIALIZATION, "Precise Cuts" );

  talent.outlaw.take_em_by_surprise = find_talent_spell( talent_tree::SPECIALIZATION, "Take 'em by Surprise" );
  talent.outlaw.summarily_dispatched = find_talent_spell( talent_tree::SPECIALIZATION, "Summarily Dispatched" );
  talent.outlaw.fan_the_hammer = find_talent_spell( talent_tree::SPECIALIZATION, "Fan the Hammer" );

  talent.outlaw.hidden_opportunity = find_talent_spell( talent_tree::SPECIALIZATION, "Hidden Opportunity" );
  talent.outlaw.keep_it_rolling = find_talent_spell( talent_tree::SPECIALIZATION, "Keep it Rolling" );
  talent.outlaw.greenskins_wickers = find_talent_spell( talent_tree::SPECIALIZATION, "Greenskin's Wickers" );

  // Subtlety Talents
  talent.subtlety.improved_backstab = find_talent_spell( talent_tree::SPECIALIZATION, "Improved Backstab" );
  talent.subtlety.shadowstep = find_talent_spell( talent_tree::SPECIALIZATION, "Shadowstep", ROGUE_SUBTLETY );
  talent.subtlety.improved_shuriken_storm = find_talent_spell( talent_tree::SPECIALIZATION, "Improved Shuriken Storm" );

  talent.subtlety.weaponmaster = find_talent_spell( talent_tree::SPECIALIZATION, "Weaponmaster", ROGUE_SUBTLETY );
  talent.subtlety.shadow_focus = find_talent_spell( talent_tree::SPECIALIZATION, "Shadow Focus" );
  talent.subtlety.quick_decisions = find_talent_spell( talent_tree::SPECIALIZATION, "Quick Decisions" );
  talent.subtlety.relentless_strikes = find_talent_spell( talent_tree::SPECIALIZATION, "Relentless Strikes" );
  talent.subtlety.black_powder = find_talent_spell( talent_tree::SPECIALIZATION, "Black Powder" );

  talent.subtlety.shot_in_the_dark = find_talent_spell( talent_tree::SPECIALIZATION, "Shot in the Dark" );
  talent.subtlety.premeditation = find_talent_spell( talent_tree::SPECIALIZATION, "Premeditation" );
  talent.subtlety.shadow_blades = find_talent_spell( talent_tree::SPECIALIZATION, "Shadow Blades" );
  talent.subtlety.silent_storm = find_talent_spell( talent_tree::SPECIALIZATION, "Silent Storm" );
  talent.subtlety.night_terrors = find_talent_spell( talent_tree::SPECIALIZATION, "Night Terrors" );

  talent.subtlety.gloomblade = find_talent_spell( talent_tree::SPECIALIZATION, "Gloomblade" );
  talent.subtlety.improved_shadow_techniques = find_talent_spell( talent_tree::SPECIALIZATION, "Improved Shadow Techniques" );
  talent.subtlety.stiletto_staccato = find_talent_spell( talent_tree::SPECIALIZATION, "Stiletto Staccato" );
  talent.subtlety.veiltouched = find_talent_spell( talent_tree::SPECIALIZATION, "Veiltouched" );
  talent.subtlety.secret_technique = find_talent_spell( talent_tree::SPECIALIZATION, "Secret Technique" );

  talent.subtlety.swift_death = find_talent_spell( talent_tree::SPECIALIZATION, "Swift Death" );
  talent.subtlety.master_of_shadows = find_talent_spell( talent_tree::SPECIALIZATION, "Master of Shadows" );
  talent.subtlety.deepening_shadows = find_talent_spell( talent_tree::SPECIALIZATION, "Deepening Shadows" );
  talent.subtlety.the_first_dance = find_talent_spell( talent_tree::SPECIALIZATION, "The First Dance" );
  talent.subtlety.replicating_shadows = find_talent_spell( talent_tree::SPECIALIZATION, "Replicating Shadows" );

  talent.subtlety.shrouded_in_darkness = find_talent_spell( talent_tree::SPECIALIZATION, "Shrouded in Darkness" );
  talent.subtlety.planned_execution = find_talent_spell( talent_tree::SPECIALIZATION, "Planned Execution" );
  talent.subtlety.improved_shadow_dance = find_talent_spell( talent_tree::SPECIALIZATION, "Improved Shadow Dance" );
  talent.subtlety.shadowed_finishers = find_talent_spell( talent_tree::SPECIALIZATION, "Shadowed Finishers" );
  talent.subtlety.shuriken_tornado = find_talent_spell( talent_tree::SPECIALIZATION, "Shuriken Tornado" );

  talent.subtlety.inevitability = find_talent_spell( talent_tree::SPECIALIZATION, "Inevitability" );
  talent.subtlety.without_a_trace = find_talent_spell( talent_tree::SPECIALIZATION, "Without a Trace" );
  talent.subtlety.fade_to_nothing = find_talent_spell( talent_tree::SPECIALIZATION, "Fade to Nothing" );
  talent.subtlety.cloaked_in_shadow = find_talent_spell( talent_tree::SPECIALIZATION, "Cloaked in Shadow" );
  talent.subtlety.secret_stratagem = find_talent_spell( talent_tree::SPECIALIZATION, "Secret Stratagem" );

  talent.subtlety.sepsis = find_talent_spell( talent_tree::SPECIALIZATION, "Sepsis", ROGUE_SUBTLETY );
  talent.subtlety.perforated_veins = find_talent_spell( talent_tree::SPECIALIZATION, "Perforated Veins" );
  talent.subtlety.dark_shadow = find_talent_spell( talent_tree::SPECIALIZATION, "Dark Shadow" );  
  talent.subtlety.deeper_daggers = find_talent_spell( talent_tree::SPECIALIZATION, "Deeper Daggers" );
  talent.subtlety.flagellation = find_talent_spell( talent_tree::SPECIALIZATION, "Flagellation" );

  talent.subtlety.invigorating_shadowdust = find_talent_spell( talent_tree::SPECIALIZATION, "Invigorating Shadowdust" );
  talent.subtlety.lingering_shadow = find_talent_spell( talent_tree::SPECIALIZATION, "Lingering Shadow" );
  talent.subtlety.finality = find_talent_spell( talent_tree::SPECIALIZATION, "Finality" );

  talent.subtlety.the_rotten = find_talent_spell( talent_tree::SPECIALIZATION, "The Rotten" );
  talent.subtlety.danse_macabre = find_talent_spell( talent_tree::SPECIALIZATION, "Danse Macabre" );
  talent.subtlety.dark_brew = find_talent_spell( talent_tree::SPECIALIZATION, "Dark Brew" );

  // Shared Talents
  spell.shadow_dance = talent.rogue.shadow_dance.find_override_spell( false );
  spell.shadowstep = talent.rogue.shadowstep.find_override_spell( false );

  auto find_shared_talent = [ this ]( std::vector<player_talent_t*> talents ) {
    for ( const auto t : talents )
    {
      if ( t->ok() )
      {
        return *t;
      }
    }
    return *talents[ 0 ];
  };

  talent.shared.sepsis = find_shared_talent( { &talent.assassination.sepsis, &talent.outlaw.sepsis, &talent.subtlety.sepsis } );
  talent.shared.shadowstep = find_shared_talent( { &talent.assassination.shadowstep, &talent.subtlety.shadowstep } );

  // Class Background Spells
  spell.alacrity_buff = talent.rogue.alacrity->ok() ? find_spell( 193538 ) : spell_data_t::not_found();
  spell.find_weakness_debuff = talent.rogue.find_weakness->ok() ? find_spell( 316220 ) : spell_data_t::not_found();
  spell.leeching_poison_buff = talent.rogue.leeching_poison->ok() ? find_spell( 108211 ) : spell_data_t::not_found();
  spell.nightstalker_buff = talent.rogue.nightstalker->ok() ? find_spell( 130493 ) : spell_data_t::not_found();
  spell.prey_on_the_weak_debuff = talent.rogue.prey_on_the_weak->ok() ? find_spell( 255909 ) : spell_data_t::not_found();
  spell.subterfuge_buff = talent.rogue.subterfuge->ok() ? find_spell( 115192 ) : spell_data_t::not_found();
  spell.vanish_buff = spell.vanish->ok() ? find_spell( 11327 ) : spell_data_t::not_found();

  // Spec Background Spells
  spec.amplifying_poison_debuff = talent.assassination.amplifying_poison->effectN( 3 ).trigger();
  spec.blindside_buff = talent.assassination.blindside->ok() ? find_spell( 121153 ) : spell_data_t::not_found();
  spec.deadly_poison_instant = talent.assassination.deadly_poison->ok() ? find_spell( 113780 ) : spell_data_t::not_found();
  spec.elaborate_planning_buff = talent.assassination.elaborate_planning->ok() ? find_spell( 193641 ) : spell_data_t::not_found();
  spec.improved_garrote_buff = talent.assassination.improved_garrote->ok() ? find_spell( 392401 ) : spell_data_t::not_found();
  spec.improved_shiv_debuff = talent.assassination.improved_shiv->ok() ? find_spell( 319504 ) : spell_data_t::not_found();
  spec.internal_bleeding_debuff = talent.assassination.internal_bleeding->ok() ? find_spell( 154953 ) : spell_data_t::not_found();
  spec.kingsbane_buff = talent.assassination.kingsbane->ok() ? find_spell( 394095 ) : spell_data_t::not_found();
  spec.master_assassin_buff = talent.assassination.master_assassin->ok() ? find_spell( 256735 ) : spell_data_t::not_found();
  spec.poison_bomb_driver = talent.assassination.poison_bomb->ok() ? find_spell( 255545 ) : spell_data_t::not_found();
  spec.poison_bomb_damage = talent.assassination.poison_bomb->ok() ? find_spell( 255546 ) : spell_data_t::not_found();
  spec.scent_of_blood_buff = talent.assassination.scent_of_blood->ok() ? find_spell( 394080 ) : spell_data_t::not_found();
  spec.vicious_venoms_ambush = talent.assassination.vicious_venoms->ok() ? find_spell( 385794 ) : spell_data_t::not_found();
  spec.vicious_venoms_mutilate_mh = talent.assassination.vicious_venoms->ok() ? find_spell( 385806 ) : spell_data_t::not_found();
  spec.vicious_venoms_mutilate_oh = talent.assassination.vicious_venoms->ok() ? find_spell( 385802 ) : spell_data_t::not_found();

  spec.deathmark_debuff = talent.assassination.deathmark->effectN( 2 ).trigger();
  spec.deathmark_amplifying_poison = talent.assassination.deathmark->ok() ? find_spell( 394328 ) : spell_data_t::not_found();
  spec.deathmark_deadly_poison_dot = talent.assassination.deathmark->ok() ? find_spell( 394324 ) : spell_data_t::not_found();
  spec.deathmark_deadly_poison_instant = talent.assassination.deathmark->ok() ? find_spell( 394325 ) : spell_data_t::not_found();
  spec.deathmark_garrote = talent.assassination.deathmark->ok() ? find_spell( 360830 ) : spell_data_t::not_found();
  spec.deathmark_instant_poison = talent.assassination.deathmark->ok() ? find_spell( 394326 ) : spell_data_t::not_found();
  spec.deathmark_rupture = talent.assassination.deathmark->ok() ? find_spell( 360826 ) : spell_data_t::not_found();
  spec.deathmark_wound_poison = talent.assassination.deathmark->ok() ? find_spell( 394327 ) : spell_data_t::not_found();

  spec.audacity_buff = talent.outlaw.audacity->ok() ? find_spell( 386270 ) : spell_data_t::not_found();
  spec.blade_flurry_attack = talent.outlaw.blade_flurry->ok() ? find_spell( 22482 ) : spell_data_t::not_found();
  spec.blade_flurry_instant_attack = talent.outlaw.blade_flurry->ok() ? find_spell( 331850 ) : spell_data_t::not_found();
  spec.blade_rush_attack = talent.outlaw.blade_rush->ok() ? find_spell( 271881 ) : spell_data_t::not_found();
  spec.blade_rush_energize = talent.outlaw.blade_rush->ok() ? find_spell( 271896 ) : spell_data_t::not_found();
  spec.hidden_opportunity_extra_attack = talent.outlaw.hidden_opportunity->ok() ? find_spell( 385897 ) : spell_data_t::not_found();
  spec.improved_adrenaline_rush_energize = talent.outlaw.improved_adrenaline_rush->ok() ? find_spell( 395424 ) : spell_data_t::not_found();
  spec.killing_spree_mh_attack = talent.outlaw.killing_spree->ok() ? find_spell( 57841 ) : spell_data_t::not_found();
  spec.killing_spree_oh_attack = spec.killing_spree_mh_attack->effectN( 1 ).trigger();
  spec.opportunity_buff = talent.outlaw.opportunity->ok() ? find_spell( 195627 ) : spell_data_t::not_found();
  spec.sinister_strike_extra_attack = talent.outlaw.opportunity->ok() ? find_spell( 197834 ) : spell_data_t::not_found();
  spec.summarily_dispatched_buff = talent.outlaw.summarily_dispatched->ok() ? find_spell( 386868 ) : spell_data_t::not_found();
  spec.take_em_by_surprise_buff = talent.outlaw.take_em_by_surprise->ok() ? find_spell( 385907 ) : spell_data_t::not_found();

  spec.broadside = talent.outlaw.roll_the_bones->ok() ? find_spell( 193356 ) : spell_data_t::not_found();
  spec.buried_treasure = talent.outlaw.roll_the_bones->ok() ? find_spell( 199600 ) : spell_data_t::not_found();
  spec.grand_melee = talent.outlaw.roll_the_bones->ok() ? find_spell( 193358 ) : spell_data_t::not_found();
  spec.skull_and_crossbones = talent.outlaw.roll_the_bones->ok() ? find_spell( 199603 ) : spell_data_t::not_found();
  spec.ruthless_precision = talent.outlaw.roll_the_bones->ok() ? find_spell( 193357 ) : spell_data_t::not_found();
  spec.true_bearing = talent.outlaw.roll_the_bones->ok() ? find_spell( 193359 ) : spell_data_t::not_found();

  spec.black_powder_shadow_attack = talent.subtlety.shadowed_finishers->ok() ? find_spell( 319190 ) : spell_data_t::not_found();
  spec.danse_macabre_buff = talent.subtlety.danse_macabre->ok() ? find_spell( 393969 ) : spell_data_t::not_found();
  spec.eviscerate_shadow_attack = talent.subtlety.shadowed_finishers->ok() ? find_spell( 328082 ) : spell_data_t::not_found();
  spec.master_of_shadows_buff = talent.subtlety.master_of_shadows->ok() ? find_spell( 196980 ) : spell_data_t::not_found();
  spec.lingering_shadow_attack = talent.subtlety.lingering_shadow->ok() ? find_spell( 386081 ) : spell_data_t::not_found();
  spec.lingering_shadow_buff = talent.subtlety.lingering_shadow->ok() ? find_spell( 385960 ) : spell_data_t::not_found();
  spec.premeditation_buff = talent.subtlety.premeditation->ok() ? find_spell( 343173 ) : spell_data_t::not_found();
  spec.relentless_strikes_energize = talent.subtlety.relentless_strikes->ok() ? find_spell( 98440 ) : spell_data_t::not_found();
  spec.replicating_shadows_tick = talent.subtlety.replicating_shadows->ok() ? find_spell( 394031 ) : spell_data_t::not_found();
  spec.secret_technique_attack = talent.subtlety.secret_technique->ok() ? find_spell( 280720 ) : spell_data_t::not_found();
  spec.secret_technique_clone_attack = talent.subtlety.secret_technique->ok() ? find_spell( 282449 ) : spell_data_t::not_found();
  spec.shadowstrike_stealth_buff = spec.shadowstrike->ok() ? find_spell( 196911 ) : spell_data_t::not_found();
  spec.shadow_blades_attack = talent.subtlety.shadow_blades->ok() ? find_spell( 279043 ) : spell_data_t::not_found();
  spec.shadow_focus_buff = talent.subtlety.shadow_focus->ok() ? find_spell( 112942 ) : spell_data_t::not_found();
  spec.shadow_techniques_energize = spec.shadow_techniques->ok() ? find_spell( 196911 ) : spell_data_t::not_found();
  spec.shot_in_the_dark_buff = talent.subtlety.shot_in_the_dark->ok() ? find_spell( 257506 ) : spell_data_t::not_found();
  spec.silent_storm_buff = talent.subtlety.silent_storm->ok() ? find_spell( 385722 ) : spell_data_t::not_found();

  // Covenant Abilities =====================================================

  covenant.echoing_reprimand      = find_covenant_spell( "Echoing Reprimand" );
  covenant.flagellation           = find_covenant_spell( "Flagellation" );
  covenant.sepsis                 = find_covenant_spell( "Sepsis" );
  covenant.serrated_bone_spike    = find_covenant_spell( "Serrated Bone Spike" );

  // Conduits ===============================================================

  conduit.lethal_poisons          = find_conduit_spell( "Lethal Poisons" );
  conduit.maim_mangle             = find_conduit_spell( "Maim, Mangle" );
  conduit.poisoned_katar          = find_conduit_spell( "Poisoned Katar");
  conduit.well_placed_steel       = find_conduit_spell( "Well-Placed Steel" );

  conduit.ambidexterity           = find_conduit_spell( "Ambidexterity" );
  conduit.count_the_odds          = find_conduit_spell( "Count the Odds" );
  conduit.sleight_of_hand         = find_conduit_spell( "Sleight of Hand" );
  conduit.triple_threat           = find_conduit_spell( "Triple Threat" );

  conduit.deeper_daggers          = find_conduit_spell( "Deeper Daggers" );
  conduit.perforated_veins        = find_conduit_spell( "Perforated Veins" );
  conduit.planned_execution       = find_conduit_spell( "Planned Execution" );
  conduit.stiletto_staccato       = find_conduit_spell( "Stiletto Staccato" );

  conduit.lashing_scars           = find_conduit_spell( "Lashing Scars" );
  conduit.reverberation           = find_conduit_spell( "Reverberation" );
  conduit.sudden_fractures        = find_conduit_spell( "Sudden Fractures" );
  conduit.septic_shock            = find_conduit_spell( "Septic Shock" );

  conduit.recuperator             = find_conduit_spell( "Recuperator" );

  // Set Bonus Items ========================================================

  set_bonuses.t29_assassination_2pc = sets->set( ROGUE_ASSASSINATION, T29, B2 );
  set_bonuses.t29_assassination_4pc = sets->set( ROGUE_ASSASSINATION, T29, B4 );
  set_bonuses.t29_outlaw_2pc        = sets->set( ROGUE_OUTLAW, T29, B2 );
  set_bonuses.t29_outlaw_4pc        = sets->set( ROGUE_OUTLAW, T29, B4 );
  set_bonuses.t29_subtlety_2pc      = sets->set( ROGUE_SUBTLETY, T29, B2 );
  set_bonuses.t29_subtlety_4pc      = sets->set( ROGUE_SUBTLETY, T29, B4 );

  // Legendary Items ========================================================

  // Generic
  legendary.essence_of_bloodfang      = find_runeforge_legendary( "Essence of Bloodfang" );
  legendary.master_assassins_mark     = find_runeforge_legendary( "Mark of the Master Assassin" );
  legendary.tiny_toxic_blade          = find_runeforge_legendary( "Tiny Toxic Blade" );
  legendary.invigorating_shadowdust   = find_runeforge_legendary( "Invigorating Shadowdust" );
  
  // Generic Covenant
  legendary.deathspike                = find_runeforge_legendary( "Deathspike" );
  legendary.obedience                 = find_runeforge_legendary( "Obedience" );
  legendary.resounding_clarity        = find_runeforge_legendary( "Resounding Clarity" );
  legendary.toxic_onslaught           = find_runeforge_legendary( "Toxic Onslaught" );

  // Assassination
  legendary.dashing_scoundrel         = find_runeforge_legendary( "Dashing Scoundrel" );
  legendary.doomblade                 = find_runeforge_legendary( "Doomblade" );
  legendary.duskwalkers_patch         = find_runeforge_legendary( "Duskwalker's Patch" );
  legendary.zoldyck_insignia          = find_runeforge_legendary( "Zoldyck Insignia" );

  // Outlaw
  legendary.greenskins_wickers        = find_runeforge_legendary( "Greenskin's Wickers" );
  legendary.guile_charm               = find_runeforge_legendary( "Guile Charm" );
  legendary.celerity                  = find_runeforge_legendary( "Celerity" );
  legendary.concealed_blunderbuss     = find_runeforge_legendary( "Concealed Blunderbuss" );

  // Subtlety
  legendary.akaaris_soul_fragment     = find_runeforge_legendary( "Akaari's Soul Fragment" );
  legendary.deathly_shadows           = find_runeforge_legendary( "Deathly Shadows" );
  legendary.finality                  = find_runeforge_legendary( "Finality" );
  legendary.the_rotten                = find_runeforge_legendary( "The Rotten" );

  // Cross-Expansion Spell Overrides ========================================

  // Assassination
  spec.dashing_scoundrel = ( talent.assassination.dashing_scoundrel->ok() ? 
                             talent.assassination.dashing_scoundrel : legendary.dashing_scoundrel );
  if ( spec.dashing_scoundrel->ok() )
  {
    spec.dashing_scoundrel_gain = find_spell( 340426 )->effectN( 1 ).resource( RESOURCE_ENERGY );
  }

  if ( talent.assassination.doomblade->ok() )
    spec.doomblade_debuff = find_spell( 381672 );
  else if ( legendary.doomblade->ok() )
    spec.doomblade_debuff = find_spell( 340431 );
  else
    spec.doomblade_debuff = spell_data_t::not_found();

  if ( covenant.serrated_bone_spike->ok() || talent.assassination.serrated_bone_spike->ok() )
    spec.serrated_bone_spike_energize = find_spell( 328548 );
  else
    spec.serrated_bone_spike_energize = spell_data_t::not_found();

  spec.zoldyck_insignia = ( talent.assassination.zoldyck_recipe->ok() ?
                            talent.assassination.zoldyck_recipe : legendary.zoldyck_insignia );

  // Outlaw
  
  if ( conduit.triple_threat->ok() || talent.outlaw.triple_threat->ok() )
    spec.triple_threat_attack = find_spell( 341541 );
  else
    spec.triple_threat_attack = spell_data_t::not_found();

  spec.greenskins_wickers = ( talent.outlaw.greenskins_wickers->ok() ?
                              talent.outlaw.greenskins_wickers : legendary.greenskins_wickers );

  if ( talent.outlaw.greenskins_wickers->ok() )
    spec.greenskins_wickers_buff = find_spell( 394131 );
  else if ( legendary.greenskins_wickers->ok() )
    spec.greenskins_wickers_buff = find_spell( 340573 );
  else
    spec.greenskins_wickers_buff = spell_data_t::not_found();

  // Subtlety

  if ( talent.subtlety.deeper_daggers->ok() )
    spec.deeper_daggers_buff = talent.subtlety.deeper_daggers->effectN( 1 ).trigger();
  else if ( conduit.perforated_veins.ok() )
    spec.deeper_daggers_buff = conduit.deeper_daggers->effectN( 1 ).trigger();
  else
    spec.deeper_daggers_buff = spell_data_t::not_found();

  if ( talent.subtlety.finality->ok() )
  {
    spec.finality_black_powder_buff = find_spell( 385948 );
    spec.finality_eviscerate_buff = find_spell( 385949 );
    spec.finality_rupture_buff = find_spell( 385951 );
  }
  else if ( legendary.finality->ok() )
  {
    spec.finality_black_powder_buff = find_spell( 340603 );
    spec.finality_eviscerate_buff = find_spell( 340600 );
    spec.finality_rupture_buff = find_spell( 340601 );
  }
  else
  {
    spec.finality_black_powder_buff = spell_data_t::not_found();
    spec.finality_eviscerate_buff = spell_data_t::not_found();
    spec.finality_rupture_buff = spell_data_t::not_found();
  }

  if ( talent.subtlety.flagellation->ok() )
  {
    spec.flagellation_buff = talent.subtlety.flagellation;
    spec.flagellation_persist_buff = find_spell( 394758 );
    spec.flagellation_damage = find_spell( 394757 );

  }
  else if ( covenant.flagellation->ok() )
  {
    spec.flagellation_buff = covenant.flagellation;
    spec.flagellation_persist_buff = find_spell( 345569 );
    spec.flagellation_damage = find_spell( 345316 );
  }
  else
  {
    spec.flagellation_buff = spell_data_t::not_found();
    spec.flagellation_persist_buff = spell_data_t::not_found();
    spec.flagellation_damage = spell_data_t::not_found();
  }

  if ( talent.subtlety.invigorating_shadowdust->ok() )
    spec.invigorating_shadowdust_cdr = talent.subtlety.invigorating_shadowdust;
  else if ( legendary.invigorating_shadowdust->ok() )
    spec.invigorating_shadowdust_cdr = find_spell( 340080 );
  else
    spec.invigorating_shadowdust_cdr = spell_data_t::not_found();

  if ( talent.subtlety.invigorating_shadowdust->ok() )
    spec.invigorating_shadowdust_cdr = talent.subtlety.invigorating_shadowdust;
  else if ( legendary.invigorating_shadowdust->ok() )
    spec.invigorating_shadowdust_cdr = find_spell( 340080 );
  else
    spec.invigorating_shadowdust_cdr = spell_data_t::not_found();

  if ( talent.subtlety.perforated_veins->ok() )
    spec.perforated_veins_buff = talent.subtlety.perforated_veins->effectN( 1 ).trigger();
  else if ( conduit.perforated_veins.ok() )
    spec.perforated_veins_buff = conduit.perforated_veins->effectN( 1 ).trigger();
  else
    spec.perforated_veins_buff = spell_data_t::not_found();

  // Generic
  spell.echoing_reprimand = ( talent.rogue.echoing_reprimand->ok() ?
                              talent.rogue.echoing_reprimand : covenant.echoing_reprimand );

  spell.sepsis_buff = ( ( covenant.sepsis->ok() || talent.shared.sepsis->ok() ) ?
                          find_spell( 347037 ) : spell_data_t::not_found() );

  if ( talent.shared.sepsis->ok() )
    spell.sepsis_expire_damage = find_spell( 394026 );
  else if ( covenant.sepsis->ok() )
    spell.sepsis_expire_damage = find_spell( 328306 );
  else
    spell.sepsis_expire_damage = spell_data_t::not_found();

  // Active Spells ==========================================================

  auto_attack = new actions::auto_melee_attack_t( this, "" );

  // Assassination
  if ( talent.assassination.deathmark->ok() )
  {
    active.deathmark.garrote = get_secondary_trigger_action<actions::garrote_t>(
      secondary_trigger::DEATHMARK, "garrote_deathmark", spec.deathmark_garrote );
    active.deathmark.rupture = get_secondary_trigger_action<actions::rupture_t>(
      secondary_trigger::DEATHMARK, "rupture_deathmark", spec.deathmark_rupture );

    active.deathmark.amplifying_poison = get_secondary_trigger_action<actions::amplifying_poison_t::amplifying_poison_dd_t>(
      secondary_trigger::DEATHMARK, "amplifying_poison_deathmark", spec.deathmark_amplifying_poison );
    active.deathmark.deadly_poison_dot = get_secondary_trigger_action<actions::deadly_poison_t::deadly_poison_dot_t>(
      secondary_trigger::DEATHMARK, "deadly_poison_dot_deathmark", spec.deathmark_deadly_poison_dot );
    active.deathmark.deadly_poison_instant = get_secondary_trigger_action<actions::deadly_poison_t::deadly_poison_dd_t>(
      secondary_trigger::DEATHMARK, "deadly_poison_instant_deathmark", spec.deathmark_deadly_poison_instant );
    active.deathmark.instant_poison = get_secondary_trigger_action<actions::instant_poison_t::instant_poison_dd_t>(
      secondary_trigger::DEATHMARK, "instant_poison_deathmark", spec.deathmark_instant_poison );
    active.deathmark.wound_poison = get_secondary_trigger_action<actions::wound_poison_t::wound_poison_dd_t>(
      secondary_trigger::DEATHMARK, "wound_poison_deathmark", spec.deathmark_wound_poison );
  }

  if ( talent.assassination.vicious_venoms->ok() )
  {
    active.vicious_venoms.ambush = get_secondary_trigger_action<actions::vicious_venoms_t>(
      secondary_trigger::VICIOUS_VENOMS, "ambush_vicious_venoms", spec.vicious_venoms_ambush );
    active.vicious_venoms.mutilate_mh = get_secondary_trigger_action<actions::vicious_venoms_t>(
      secondary_trigger::VICIOUS_VENOMS, "mutilate_mh_vicious_venoms", spec.vicious_venoms_mutilate_mh );
    active.vicious_venoms.mutilate_oh = get_secondary_trigger_action<actions::vicious_venoms_t>(
      secondary_trigger::VICIOUS_VENOMS, "mutilate_oh_vicious_venoms", spec.vicious_venoms_mutilate_oh );
    active.vicious_venoms.mutilate_oh->weapon = &( off_hand_weapon ); // TOCHECK -- Not in spell data
  }

  if ( talent.assassination.poison_bomb->ok() )
  {
    active.poison_bomb = get_background_action<actions::poison_bomb_t>( "poison_bomb" );
  }

  // Outlaw
  if ( mastery.main_gauche->ok() )
  {
    active.main_gauche = get_secondary_trigger_action<actions::main_gauche_t>(
      secondary_trigger::MAIN_GAUCHE, "main_gauche" );
  }

  if ( spec.blade_flurry_attack->ok() )
  {
    active.blade_flurry = get_background_action<actions::blade_flurry_attack_t>( "blade_flurry_attack" );
  }

  if ( conduit.triple_threat.ok() && specialization() == ROGUE_OUTLAW || talent.outlaw.triple_threat->ok() )
  {
    active.triple_threat_mh = get_secondary_trigger_action<actions::sinister_strike_t::sinister_strike_extra_attack_t>(
      secondary_trigger::TRIPLE_THREAT, "sinister_strike_triple_threat_mh" );
    active.triple_threat_oh = get_secondary_trigger_action<actions::sinister_strike_t::triple_threat_t>(
      secondary_trigger::TRIPLE_THREAT, "sinister_strike_triple_threat_oh" );
  }

  if ( talent.outlaw.fan_the_hammer->ok() )
  {
    active.fan_the_hammer = get_secondary_trigger_action<actions::pistol_shot_t>(
      secondary_trigger::FAN_THE_HAMMER, "pistol_shot_fan_the_hammer" );
  }
  else if ( legendary.concealed_blunderbuss->ok() )
  {
    active.concealed_blunderbuss = get_secondary_trigger_action<actions::pistol_shot_t>(
      secondary_trigger::CONCEALED_BLUNDERBUSS, "pistol_shot_concealed_blunderbuss" );
  }

  // Subtlety
  if ( talent.subtlety.weaponmaster->ok() )
  {
    cooldowns.weaponmaster->base_duration = talent.subtlety.weaponmaster->internal_cooldown();
    active.weaponmaster.backstab = get_secondary_trigger_action<actions::backstab_t>(
      secondary_trigger::WEAPONMASTER, "backstab_weaponmaster" );
    active.weaponmaster.gloomblade = get_secondary_trigger_action<actions::gloomblade_t>(
      secondary_trigger::WEAPONMASTER, "gloomblade_weaponmaster" );
    active.weaponmaster.shadowstrike = get_secondary_trigger_action<actions::shadowstrike_t>(
      secondary_trigger::WEAPONMASTER, "shadowstrike_weaponmaster" );
    active.weaponmaster.akaaris_shadowstrike = get_secondary_trigger_action<actions::akaaris_shadowstrike_t>(
      secondary_trigger::WEAPONMASTER, "shadowstrike_akaaris_weaponmaster" );
  }

  if ( talent.subtlety.lingering_shadow->ok() )
  {
    active.lingering_shadow = get_background_action<actions::lingering_shadow_t>( "lingering_shadow" );
  }

  if ( specialization() == ROGUE_SUBTLETY || legendary.toxic_onslaught->ok() )
  {
    active.shadow_blades_attack = get_background_action<actions::shadow_blades_attack_t>( "shadow_blades_attack" );
  }

  if ( spec.flagellation_damage->ok() )
  {
    active.flagellation = get_secondary_trigger_action<actions::flagellation_damage_t>(
      secondary_trigger::FLAGELLATION, "flagellation_damage" );
  }

  // Shadowlands
  if ( legendary.essence_of_bloodfang->ok() )
  {
    active.bloodfang = get_background_action<actions::bloodfang_t>( "bloodfang" );
  }

  if ( legendary.akaaris_soul_fragment->ok() )
  {
    active.akaaris_shadowstrike = get_secondary_trigger_action<actions::akaaris_shadowstrike_t>(
      secondary_trigger::AKAARIS_SOUL_FRAGMENT, "shadowstrike_akaaris" );
  }
}

// rogue_t::init_gains ======================================================

void rogue_t::init_gains()
{
  player_t::init_gains();

  gains.adrenaline_rush           = get_gain( "Adrenaline Rush" );
  gains.adrenaline_rush_expiry    = get_gain( "Adrenaline Rush (Expiry)" );
  gains.blade_rush                = get_gain( "Blade Rush" );
  gains.broadside                 = get_gain( "Broadside" );
  gains.buried_treasure           = get_gain( "Buried Treasure" );
  gains.fatal_flourish            = get_gain( "Fatal Flourish" );
  gains.dashing_scoundrel         = get_gain( "Dashing Scoundrel" );
  gains.deathly_shadows           = get_gain( "Deathly Shadows" );
  gains.dreadblades               = get_gain( "Dreadblades" );
  gains.energy_refund             = get_gain( "Energy Refund" );
  gains.master_of_shadows         = get_gain( "Master of Shadows" );
  gains.premeditation             = get_gain( "Premeditation" );
  gains.quick_draw                = get_gain( "Quick Draw" );
  gains.relentless_strikes        = get_gain( "Relentless Strikes" );
  gains.ruthlessness              = get_gain( "Ruthlessness" );
  gains.seal_fate                 = get_gain( "Seal Fate" );
  gains.serrated_bone_spike       = get_gain( "Serrated Bone Spike (DoT)" );
  gains.shadow_blades             = get_gain( "Shadow Blades" );
  gains.shadow_techniques         = get_gain( "Shadow Techniques" );
  gains.slice_and_dice            = get_gain( "Slice and Dice" );
  gains.symbols_of_death          = get_gain( "Symbols of Death" );
  gains.the_rotten                = get_gain( "The Rotten" );
  gains.venom_rush                = get_gain( "Venom Rush" );
  gains.venomous_wounds           = get_gain( "Venomous Vim" );
  gains.venomous_wounds_death     = get_gain( "Venomous Vim (Death)" );

  gains.ace_up_your_sleeve              = get_gain( "Ace Up Your Sleeve" );
  gains.improved_adrenaline_rush        = get_gain( "Improved Adrenaline Rush" );
  gains.improved_adrenaline_rush_expiry = get_gain( "Improved Adrenaline Rush (Expiry)" );
  gains.improved_ambush                 = get_gain( "Improved Ambush" );
  gains.shrouded_suffocation            = get_gain( "Shrouded Suffocation" );
  gains.the_first_dance                 = get_gain( "The First Dance " );
}

// rogue_t::init_procs ======================================================

void rogue_t::init_procs()
{
  player_t::init_procs();

  procs.weaponmaster        = get_proc( "Weaponmaster"                 );

  procs.roll_the_bones_1    = get_proc( "Roll the Bones: 1 buff"       );
  procs.roll_the_bones_2    = get_proc( "Roll the Bones: 2 buffs"      );
  procs.roll_the_bones_3    = get_proc( "Roll the Bones: 3 buffs"      );
  procs.roll_the_bones_4    = get_proc( "Roll the Bones: 4 buffs"      );
  procs.roll_the_bones_5    = get_proc( "Roll the Bones: 5 buffs"      );
  procs.roll_the_bones_6    = get_proc( "Roll the Bones: 6 buffs"      );
  static_cast<buffs::roll_the_bones_t*>( buffs.roll_the_bones )->set_procs();

  procs.deepening_shadows   = get_proc( "Deepening Shadows"            );

  procs.echoing_reprimand.clear();
  procs.echoing_reprimand.push_back( get_proc( "Animacharged CP 2 Used" ) );
  procs.echoing_reprimand.push_back( get_proc( "Animacharged CP 3 Used" ) );
  procs.echoing_reprimand.push_back( get_proc( "Animacharged CP 4 Used" ) );
  procs.echoing_reprimand.push_back( get_proc( "Animacharged CP 5 Used" ) );

  procs.flagellation_cp_spend = get_proc( "CP Spent During Flagellation" );

  procs.serrated_bone_spike_refund            = get_proc( "Serrated Bone Spike Refund" );
  procs.serrated_bone_spike_waste             = get_proc( "Serrated Bone Spike Refund Wasted" );
  procs.serrated_bone_spike_waste_partial     = get_proc( "Serrated Bone Spike Refund Wasted (Partial)" );

  procs.count_the_odds          = get_proc( "Count the Odds" );
  procs.count_the_odds_capped   = get_proc( "Count the Odds Capped" );
  procs.roll_the_bones_wasted   = get_proc( "Roll the Bones Wasted" );

  procs.duskwalker_patch    = get_proc( "Duskwalker Patch"             );

  procs.amplifying_poison_consumed           = get_proc( "Amplifying Poison Consumed" );
  procs.amplifying_poison_deathmark_consumed = get_proc( "Amplifying Poison (Deathmark) Consumed" );
}

// rogue_t::init_scaling ====================================================

void rogue_t::init_scaling()
{
  player_t::init_scaling();

  scaling -> enable( STAT_WEAPON_OFFHAND_DPS );
  scaling -> disable( STAT_STRENGTH );

  // Break out early if scaling is disabled on this player, or there's no
  // scaling stat
  if ( ! scale_player || sim -> scaling -> scale_stat == STAT_NONE )
  {
    return;
  }

  // If weapon swapping is used, adjust the weapon_t object damage values in the weapon state
  // information if this simulator is scaling the corresponding weapon DPS (main or offhand). This
  // is necessary, as weapon swapping overwrites player_t::main_hand_weapon and
  // player_t::ofF_hand_weapon, which is where player_t::init_scaling originally injects the
  // increased scaling value.
  if ( sim -> scaling -> scale_stat == STAT_WEAPON_DPS &&
       weapon_data[ WEAPON_MAIN_HAND ].secondary_weapon_data.active() )
  {
    double v = sim -> scaling -> scale_value;
    double pvalue = weapon_data[ WEAPON_MAIN_HAND ].weapon_data[ WEAPON_PRIMARY ].swing_time.total_seconds() * v;
    double svalue = weapon_data[ WEAPON_MAIN_HAND ].weapon_data[ WEAPON_SECONDARY ].swing_time.total_seconds() * v;

    weapon_data[ WEAPON_MAIN_HAND ].weapon_data[ WEAPON_PRIMARY ].damage += pvalue;
    weapon_data[ WEAPON_MAIN_HAND ].weapon_data[ WEAPON_PRIMARY ].min_dmg += pvalue;
    weapon_data[ WEAPON_MAIN_HAND ].weapon_data[ WEAPON_PRIMARY ].max_dmg += pvalue;

    weapon_data[ WEAPON_MAIN_HAND ].weapon_data[ WEAPON_SECONDARY ].damage += svalue;
    weapon_data[ WEAPON_MAIN_HAND ].weapon_data[ WEAPON_SECONDARY ].min_dmg += svalue;
    weapon_data[ WEAPON_MAIN_HAND ].weapon_data[ WEAPON_SECONDARY ].max_dmg += svalue;
  }

  if ( sim -> scaling -> scale_stat == STAT_WEAPON_OFFHAND_DPS &&
       weapon_data[ WEAPON_OFF_HAND ].secondary_weapon_data.active() )
  {
    double v = sim -> scaling -> scale_value;
    double pvalue = weapon_data[ WEAPON_OFF_HAND ].weapon_data[ WEAPON_PRIMARY ].swing_time.total_seconds() * v;
    double svalue = weapon_data[ WEAPON_OFF_HAND ].weapon_data[ WEAPON_SECONDARY ].swing_time.total_seconds() * v;

    weapon_data[ WEAPON_OFF_HAND ].weapon_data[ WEAPON_PRIMARY ].damage += pvalue;
    weapon_data[ WEAPON_OFF_HAND ].weapon_data[ WEAPON_PRIMARY ].min_dmg += pvalue;
    weapon_data[ WEAPON_OFF_HAND ].weapon_data[ WEAPON_PRIMARY ].max_dmg += pvalue;

    weapon_data[ WEAPON_OFF_HAND ].weapon_data[ WEAPON_SECONDARY ].damage += svalue;
    weapon_data[ WEAPON_OFF_HAND ].weapon_data[ WEAPON_SECONDARY ].min_dmg += svalue;
    weapon_data[ WEAPON_OFF_HAND ].weapon_data[ WEAPON_SECONDARY ].max_dmg += svalue;
  }
}

// rogue_t::init_resources =================================================

void rogue_t::init_resources( bool force )
{
  player_t::init_resources( force );

  resources.current[ RESOURCE_COMBO_POINT ] = options.initial_combo_points;
}

// rogue_t::init_buffs ======================================================

void rogue_t::create_buffs()
{
  player_t::create_buffs();

  // Baseline ===============================================================
  // Shared

  buffs.feint = make_buff( this, "feint", talent.rogue.feint )
    ->set_cooldown( timespan_t::zero() )
    ->set_duration( talent.rogue.feint->duration() );

  buffs.shadowstep = make_buff( this, "shadowstep", talent.rogue.shadowstep )
    ->set_cooldown( timespan_t::zero() );

  buffs.sprint = make_buff( this, "sprint", spell.sprint )
    ->set_cooldown( timespan_t::zero() )
    ->add_invalidate( CACHE_RUN_SPEED );

  buffs.slice_and_dice = new buffs::slice_and_dice_t( this );
  buffs.stealth = new buffs::stealth_t( this );
  buffs.vanish = new buffs::vanish_t( this );

  // Assassination ==========================================================

  buffs.envenom = make_buff( this, "envenom", spec.envenom )
    ->set_default_value_from_effect_type( A_ADD_FLAT_MODIFIER, P_PROC_CHANCE )
    ->set_duration( timespan_t::min() )
    ->set_period( timespan_t::zero() )
    ->set_refresh_behavior( buff_refresh_behavior::PANDEMIC );

  // Outlaw =================================================================

  buffs.adrenaline_rush = new buffs::adrenaline_rush_t( this );
  buffs.blade_flurry = new buffs::blade_flurry_t( this );

  buffs.blade_rush = make_buff( this, "blade_rush", spec.blade_rush_energize )
    ->set_default_value_from_effect_type( A_PERIODIC_ENERGIZE )
    ->set_tick_callback( [ this ]( buff_t* b, int, timespan_t ) {
      resource_gain( RESOURCE_ENERGY, b->check_value(), gains.blade_rush );
    } );

  buffs.opportunity = make_buff( this, "opportunity", spec.opportunity_buff )
    ->set_default_value_from_effect_type( A_ADD_PCT_MODIFIER, P_GENERIC )
    ->apply_affecting_aura( talent.outlaw.quick_draw )
    ->apply_affecting_aura( talent.outlaw.fan_the_hammer );

  buffs.take_em_by_surprise = make_buff( this, "take_em_by_surprise", spec.take_em_by_surprise_buff )
    ->set_default_value_from_effect_type( A_HASTE_ALL )
    ->add_invalidate( CACHE_HASTE )
    ->set_duration( timespan_t::from_seconds( talent.outlaw.take_em_by_surprise->effectN( 1 ).base_value() ) );
  buffs.take_em_by_surprise_aura = make_buff( this, "take_em_by_surprise_aura", spec.take_em_by_surprise_buff )
    ->set_default_value_from_effect_type( A_HASTE_ALL )
    ->add_invalidate( CACHE_HASTE )
    ->set_constant_behavior( buff_constant_behavior::NEVER_CONSTANT )
    ->set_duration( sim->max_time / 2 ) // So it appears in sample sequence table
    ->set_stack_change_callback( [this]( buff_t*, int, int new_ ) {
    if ( new_ == 0 )
      buffs.take_em_by_surprise->trigger();
    else
      buffs.take_em_by_surprise->expire();
  } );

  buffs.summarily_dispatched = make_buff<damage_buff_t>( this, "summarily_dispatched", spec.summarily_dispatched_buff );
  buffs.summarily_dispatched
    ->set_duration( timespan_t::from_seconds( talent.outlaw.summarily_dispatched->effectN( 3 ).base_value() ) )
    ->set_refresh_behavior( buff_refresh_behavior::DISABLED );

  // Roll the Bones Buffs
  buffs.broadside = make_buff<damage_buff_t>( this, "broadside", spec.broadside );
  buffs.broadside->set_default_value_from_effect( 1, 1.0 ); // Extra CP Gain

  buffs.buried_treasure = make_buff( this, "buried_treasure", spec.buried_treasure )
    ->set_default_value_from_effect_type( A_RESTORE_POWER )
    ->set_affects_regen( true );

  buffs.grand_melee = make_buff( this, "grand_melee", spec.grand_melee )
    ->set_default_value_from_effect( 1, 1.0 )   // SnD Extend Seconds
    ->add_invalidate( CACHE_LEECH );

  buffs.skull_and_crossbones = make_buff( this, "skull_and_crossbones", spec.skull_and_crossbones )
    ->set_default_value_from_effect( 1, 0.01 ); // Flat Modifier to Proc%

  buffs.ruthless_precision = make_buff<damage_buff_t>( this, "ruthless_precision", spec.ruthless_precision, false )
    ->set_crit_chance_mod( spec.ruthless_precision, 1 ); // Non-BtE Crit% Modifier

  buffs.true_bearing = make_buff( this, "true_bearing", spec.true_bearing )
    ->set_default_value_from_effect( 1, 0.1 );  // CDR Seconds

  buffs.roll_the_bones = new buffs::roll_the_bones_t( this );

  // Subtlety ===============================================================

  buffs.danse_macabre = make_buff<damage_buff_t>( this, "danse_macabre", spec.danse_macabre_buff );
  buffs.danse_macabre->set_refresh_behavior( buff_refresh_behavior::DISABLED );

  buffs.shadow_blades = make_buff( this, "shadow_blades", talent.subtlety.shadow_blades ) // DFALPHA TOCHECK TO Legendary
    ->set_default_value_from_effect( 1 ) // Bonus Damage%
    ->set_cooldown( timespan_t::zero() );

  buffs.shadow_dance = new buffs::shadow_dance_t( this );

  buffs.symbols_of_death = make_buff<damage_buff_t>( this, "symbols_of_death", spec.symbols_of_death );
  buffs.symbols_of_death->set_refresh_behavior( buff_refresh_behavior::PANDEMIC );
  if ( conduit.planned_execution.ok() || talent.subtlety.planned_execution->ok() )
  {
    buffs.symbols_of_death->add_invalidate( CACHE_CRIT_CHANCE );
  }

  // Talents ================================================================
  // Shared

  buffs.alacrity = make_buff( this, "alacrity", spell.alacrity_buff )
    ->set_default_value_from_effect_type( A_HASTE_ALL )
    ->set_chance( talent.rogue.alacrity->ok() )
    ->add_invalidate( CACHE_HASTE );

  buffs.cold_blood = make_buff<damage_buff_t>( this, "cold_blood", talent.rogue.cold_blood );
  buffs.cold_blood
    ->set_cooldown( timespan_t::zero() )
    ->set_constant_behavior( buff_constant_behavior::NEVER_CONSTANT )
    ->set_duration( sim->max_time / 2 );

  // DFALPHA -- This still seems very messed up and still appears to have a 50% value in data
  //            2022-10-21 -- Appears hotfixed to not use this value in latest build but unsure how
  buffs.nightstalker = make_buff<damage_buff_t>( this, "nightstalker", spell.nightstalker_buff )
    ->set_periodic_mod( spell.nightstalker_buff, 2 ); // Dummy Value
  // 2022-10-21 -- Manually overwrite to maintain whitelist until we figure out what is going on
  buffs.nightstalker->direct_mod.multiplier = 1.0 + talent.rogue.nightstalker->effectN( 1 ).percent();
  buffs.nightstalker->periodic_mod.multiplier = 1.0 + talent.rogue.nightstalker->effectN( 1 ).percent();
  // 2022-10-21 -- Appears non-functional now even though it still exists in the passive
  //  ->apply_affecting_aura( spec.subtlety_rogue ); // DFALPHA -- Seems messed up

  buffs.subterfuge = new buffs::subterfuge_t( this );

  buffs.thistle_tea = make_buff( this, "thistle_tea", talent.rogue.thistle_tea )
    ->set_cooldown( timespan_t::zero() )
    ->set_default_value_from_effect_type( A_MOD_MASTERY_PCT )
    ->set_pct_buff_type( STAT_PCT_BUFF_MASTERY );

  // Assassination

  buffs.improved_garrote = make_buff( this, "improved_garrote", spec.improved_garrote_buff )
    ->set_default_value_from_effect( 2 )
    ->add_invalidate( CACHE_PLAYER_DAMAGE_MULTIPLIER );
  buffs.improved_garrote_aura = make_buff( this, "improved_garrote_aura", spec.improved_garrote_buff )
    ->set_default_value_from_effect( 2 )
    ->add_invalidate( CACHE_PLAYER_DAMAGE_MULTIPLIER )
    ->set_constant_behavior( buff_constant_behavior::NEVER_CONSTANT )
    ->set_duration( sim->max_time / 2 )
    ->set_stack_change_callback( [this]( buff_t*, int, int new_ ) {
      if ( new_ == 0 )
        buffs.improved_garrote->trigger();
      else
        buffs.improved_garrote->expire();
    } );

  buffs.blindside = make_buff( this, "blindside", spec.blindside_buff )
    ->set_default_value_from_effect_type( A_ADD_PCT_MODIFIER, P_RESOURCE_COST );

  buffs.elaborate_planning = make_buff<damage_buff_t>( this, "elaborate_planning", spec.elaborate_planning_buff,
                                                       talent.assassination.elaborate_planning->effectN( 1 ).percent() );

  // Cooldown on Indiscriminate Carnage starts when both buffs are used
  buffs.indiscriminate_carnage_garrote = make_buff( this, "indiscriminate_carnage_garrote",
                                                    talent.assassination.indiscriminate_carnage )
    ->set_cooldown( timespan_t::zero() )
    ->set_stack_change_callback( [this]( buff_t*, int, int new_ ) {
      if ( new_ == 0 && !buffs.indiscriminate_carnage_rupture->check() )
      {
        cooldowns.indiscriminate_carnage->reset( false );
        cooldowns.indiscriminate_carnage->start();
      }
    } );
  buffs.indiscriminate_carnage_rupture = make_buff( this, "indiscriminate_carnage_rupture",
                                                    talent.assassination.indiscriminate_carnage )
    ->set_cooldown( timespan_t::zero() )
    ->set_stack_change_callback( [this]( buff_t*, int, int new_ ) {
      if ( new_ == 0 && !buffs.indiscriminate_carnage_garrote->check() )
      {
        cooldowns.indiscriminate_carnage->reset( false );
        cooldowns.indiscriminate_carnage->start();
      }
    } );

  buffs.kingsbane = make_buff<damage_buff_t>( this, "kingsbane", spec.kingsbane_buff );
  buffs.kingsbane->set_refresh_behavior( buff_refresh_behavior::NONE );

  buffs.master_assassin = make_buff<damage_buff_t>( this, "master_assassin", spec.master_assassin_buff );
  buffs.master_assassin
    ->set_duration( timespan_t::from_seconds( talent.assassination.master_assassin->effectN( 1 ).base_value() ) );
  buffs.master_assassin_aura = make_buff<damage_buff_t>( this, "master_assassin_aura", spec.master_assassin_buff );
  buffs.master_assassin_aura
    ->set_constant_behavior( buff_constant_behavior::NEVER_CONSTANT )
    ->set_duration( sim->max_time / 2 ) // So it appears in sample sequence table
    ->set_stack_change_callback( [ this ]( buff_t*, int, int new_ ) {
      if ( new_ == 0 )
        buffs.master_assassin->trigger();
      else
        buffs.master_assassin->expire();
    } );

  buffs.scent_of_blood = make_buff( this, "scent_of_blood", spec.scent_of_blood_buff )
    ->set_default_value_from_effect_type( A_MOD_TOTAL_STAT_PERCENTAGE )
    ->set_constant_behavior( buff_constant_behavior::NEVER_CONSTANT )
    ->set_duration( sim->max_time / 2 ) // Duration is 24s in spell data, but not enforced
    ->set_pct_buff_type( STAT_PCT_BUFF_AGILITY );

  // Outlaw

  buffs.audacity = make_buff( this, "audacity", spec.audacity_buff );

  buffs.dreadblades = make_buff( this, "dreadblades", talent.outlaw.dreadblades )
    ->set_cooldown( timespan_t::zero() )
    ->set_default_value( talent.outlaw.dreadblades->effectN( 2 ).trigger()->effectN( 1 ).resource() );

  buffs.killing_spree = make_buff( this, "killing_spree", talent.outlaw.killing_spree )
    ->set_cooldown( timespan_t::zero() )
    ->set_duration( talent.outlaw.killing_spree->duration() );

  buffs.loaded_dice = make_buff( this, "loaded_dice", talent.outlaw.loaded_dice->effectN( 1 ).trigger() );

  // Subtlety

  buffs.lingering_shadow = make_buff( this, "lingering_shadow", spec.lingering_shadow_buff )
    ->apply_affecting_aura( talent.subtlety.lingering_shadow ) // Max Stack Modifier
    ->set_default_value( 1.0 )
    ->set_tick_zero( true )
    ->set_freeze_stacks( true )
    ->set_tick_callback( [this]( buff_t* b, int, timespan_t ) {
      // This is wonky and set up with a rolling partial stack reduction so it alternates per tick
      // Otherwise we could just use the normal set_reverse_stack_count() functionality
      double stack_reduction = b->max_stack() / talent.subtlety.lingering_shadow->effectN( 3 ).base_value();
      int target_stacks = std::ceil( b->max_stack() - ( stack_reduction * b->current_tick ) ) - 1;
      make_event( sim, [ b, target_stacks ] { 
        b->decrement( b->check() - target_stacks ); 
      } );
    } );

  buffs.master_of_shadows = make_buff( this, "master_of_shadows", spec.master_of_shadows_buff )
    ->set_default_value_from_effect_type( A_PERIODIC_ENERGIZE )
    ->set_refresh_behavior( buff_refresh_behavior::DURATION )
    ->set_tick_callback( [ this ]( buff_t* b, int, timespan_t ) {
      resource_gain( RESOURCE_ENERGY, b->check_value(), gains.master_of_shadows );
    } )
    ->set_stack_change_callback( [ this ]( buff_t* b, int, int new_ ) {
      if ( new_ == 1 )
        resource_gain( RESOURCE_ENERGY, b->data().effectN( 2 ).resource(), gains.master_of_shadows );
    } );

  buffs.premeditation = make_buff( this, "premeditation", spec.premeditation_buff );

  buffs.shadow_techniques = make_buff( this, "shadow_techniques", spec.shadow_techniques )
    ->set_quiet( true )
    ->set_duration( 1_s );
  buffs.shadow_techniques->reactable = true;

  buffs.shot_in_the_dark = make_buff( this, "shot_in_the_dark", spec.shot_in_the_dark_buff );

  buffs.silent_storm = make_buff( this, "silent_storm", spec.silent_storm_buff )
    ->set_default_value_from_effect_type( A_ADD_FLAT_MODIFIER, P_CRIT )
    ->add_invalidate( CACHE_CRIT_CHANCE )
    ->set_constant_behavior( buff_constant_behavior::NEVER_CONSTANT )
    ->set_duration( sim->max_time / 2 );

  buffs.secret_technique = make_buff( this, "secret_technique", talent.subtlety.secret_technique )
    ->set_cooldown( timespan_t::zero() )
    ->set_quiet( true );

  buffs.shuriken_tornado = new buffs::shuriken_tornado_t( this );

  // Covenants ==============================================================

  buffs.flagellation = make_buff( this, "flagellation_buff", spec.flagellation_buff )
    ->set_refresh_behavior( buff_refresh_behavior::DISABLED )
    ->set_cooldown( timespan_t::zero() )
    ->set_period( timespan_t::zero() )
    ->set_stack_change_callback( [ this ]( buff_t*, int old_, int new_ ) {
        if ( new_ == 0 )
          buffs.flagellation_persist->trigger( old_, buffs.flagellation->default_value );
      } );
  buffs.flagellation_persist = make_buff( this, "flagellation_persist", spec.flagellation_persist_buff );
  if ( talent.subtlety.flagellation->ok() )
  {
    buffs.flagellation
      ->set_default_value_from_effect_type( A_MOD_MASTERY_PCT )
      ->set_pct_buff_type( STAT_PCT_BUFF_MASTERY );
    buffs.flagellation_persist
      ->set_pct_buff_type( STAT_PCT_BUFF_MASTERY );
  }
  else
  {
    buffs.flagellation->set_default_value_from_effect_type( A_HASTE_ALL, P_MAX, 0.01 );
    buffs.flagellation->add_invalidate( CACHE_HASTE );
    buffs.flagellation_persist->add_invalidate( CACHE_HASTE );
    
    // DFALPHA TOCHECK -- Does not currently seem to affect the talent version of the spell
    if ( legendary.obedience->ok() )
    {
      buffs.flagellation->add_invalidate( CACHE_VERSATILITY );
      buffs.flagellation_persist->add_invalidate( CACHE_VERSATILITY );
    }
  }

  buffs.echoing_reprimand.clear();
  buffs.echoing_reprimand.push_back( make_buff( this, "echoing_reprimand_2", spell.echoing_reprimand->ok() ?
                                         find_spell( 323558 ) : spell_data_t::not_found() )
                                          ->set_max_stack( 1 )
                                          ->set_default_value( 2 ) );
  buffs.echoing_reprimand.push_back( make_buff( this, "echoing_reprimand_3", spell.echoing_reprimand->ok() ?
                                         find_spell( 323559 ) : spell_data_t::not_found() )
                                          ->set_max_stack( 1 )
                                          ->set_default_value( 3 ) );
  buffs.echoing_reprimand.push_back( make_buff( this, "echoing_reprimand_4", spell.echoing_reprimand->ok() ?
                                         find_spell( 323560 ) : spell_data_t::not_found() )
                                          ->set_max_stack( 1 )
                                          ->set_default_value( 4 ) );
  buffs.echoing_reprimand.push_back( make_buff( this, "echoing_reprimand_5", spell.echoing_reprimand->ok() ?
                                         find_spell( 354838 ) : spell_data_t::not_found() )
                                          ->set_max_stack( 1 )
                                          ->set_default_value( 5 ) );

  buffs.sepsis = make_buff( this, "sepsis_buff", spell.sepsis_buff );
  if( covenant.sepsis->ok() )
    buffs.sepsis->set_initial_stack( as<int>( covenant.sepsis->effectN( 6 ).base_value() ) );
  else if( talent.shared.sepsis->ok() )
    buffs.sepsis->set_initial_stack( as<int>( talent.shared.sepsis->effectN( 6 ).base_value() ) );

  // Conduits ===============================================================

  if ( talent.subtlety.deeper_daggers->ok() )
  {
    buffs.deeper_daggers = make_buff<damage_buff_t>( this, "deeper_daggers", spec.deeper_daggers_buff )
      ->set_direct_mod( talent.subtlety.deeper_daggers->effectN( 1 ).percent() );
    buffs.deeper_daggers->add_invalidate( CACHE_PLAYER_DAMAGE_MULTIPLIER );
  }
  else
  {
    buffs.deeper_daggers = make_buff<damage_buff_t>( this, "deeper_daggers",
                                                     spec.deeper_daggers_buff, conduit.deeper_daggers );
  }

  if ( talent.subtlety.perforated_veins->ok() )
  {
    buffs.perforated_veins = make_buff<damage_buff_t>( this, "perforated_veins",
                                                       spec.perforated_veins_buff );
    buffs.perforated_veins->set_cooldown( timespan_t::zero() );
    cooldowns.perforated_veins->base_duration = talent.subtlety.perforated_veins->internal_cooldown();
  }
  else
  {
    buffs.perforated_veins = make_buff<damage_buff_t>( this, "perforated_veins",
                                                       spec.perforated_veins_buff,
                                                       conduit.perforated_veins );
    buffs.perforated_veins->set_cooldown( timespan_t::zero() );
    cooldowns.perforated_veins->base_duration = conduit.perforated_veins->internal_cooldown();
  }

  // Set Bonus Items ========================================================
  
  buffs.t29_assassination_4pc = make_buff<damage_buff_t>( this, "septic_wounds", set_bonuses.t29_assassination_4pc->ok() ?
                                                          find_spell( 394845 ) : spell_data_t::not_found() );
  buffs.t29_assassination_4pc->set_trigger_spell( set_bonuses.t29_assassination_4pc ); // Proc Rate

  buffs.t29_outlaw_2pc = make_buff<damage_buff_t>( this, "vicious_followup", set_bonuses.t29_outlaw_2pc->ok() ?
                                                   find_spell( 394879 ) : spell_data_t::not_found() );
  buffs.t29_outlaw_2pc->set_max_stack( consume_cp_max() );

  buffs.t29_outlaw_4pc = make_buff<damage_buff_t>( this, "brutal_opportunist", set_bonuses.t29_outlaw_4pc->ok() ?
                                                   find_spell( 394888 ) : spell_data_t::not_found() );

  buffs.t29_subtlety_2pc = make_buff<damage_buff_t>( this, "honed_blades", set_bonuses.t29_subtlety_2pc->ok() ?
                                                     find_spell( 394894 ) : spell_data_t::not_found() );
  buffs.t29_subtlety_2pc
    ->set_max_stack( consume_cp_max() );    

  // Cannot fully auto-parse as a single damage_buff_t since it has different mod for Black Powder
  const spell_data_t* t29_buff = ( set_bonuses.t29_subtlety_4pc->ok() ?
                                   find_spell( 395003 ) : spell_data_t::not_found() );
  buffs.t29_subtlety_4pc = make_buff<damage_buff_t>( this, "masterful_finish", t29_buff, false )
    ->set_direct_mod( t29_buff, 1 )
    ->set_periodic_mod( t29_buff, 3 );
  buffs.t29_subtlety_4pc_black_powder = make_buff<damage_buff_t>( this, "masterful_finish_bp", t29_buff, false )
    ->set_direct_mod( t29_buff, 2 );
  buffs.t29_subtlety_4pc_black_powder->set_quiet( true );

  // Legendary Items ========================================================

  // 2021-02-18-- Sub-specific Deathly Shadows buff added in 9.0.5
  if ( specialization() == ROGUE_SUBTLETY )
    buffs.deathly_shadows = make_buff<damage_buff_t>( this, "deathly_shadows", legendary.deathly_shadows->ok() ?
                                                      find_spell( 350964 ) : spell_data_t::not_found() );
  else
    buffs.deathly_shadows = make_buff<damage_buff_t>( this, "deathly_shadows", legendary.deathly_shadows->effectN( 1 ).trigger() );

  // 2021-02-18-- Master Assassin's Mark is whitelisted since 9.0.5
  const spell_data_t* master_assassins_mark = legendary.master_assassins_mark->ok() ? find_spell( 340094 ) : spell_data_t::not_found();
  buffs.master_assassins_mark = make_buff<damage_buff_t>( this, "master_assassins_mark", master_assassins_mark );
  buffs.master_assassins_mark
    ->set_duration( timespan_t::from_seconds( legendary.master_assassins_mark->effectN( 1 ).base_value() ) );
  buffs.master_assassins_mark_aura = make_buff<damage_buff_t>( this, "master_assassins_mark_aura", master_assassins_mark );
  buffs.master_assassins_mark_aura
    ->set_constant_behavior( buff_constant_behavior::NEVER_CONSTANT )
    ->set_duration( sim->max_time / 2 ) // So it appears in sample sequence table
    ->set_stack_change_callback( [ this ]( buff_t*, int, int new_ ) {
      if ( new_ == 0 )
        buffs.master_assassins_mark->trigger();
      else
        buffs.master_assassins_mark->expire();
  } );

  buffs.the_rotten = make_buff<damage_buff_t>( this, "the_rotten", ( talent.subtlety.the_rotten->ok() ?
                                                                     talent.subtlety.the_rotten->effectN( 1 ).trigger() :
                                                                     legendary.the_rotten->effectN( 1 ).trigger() ) );
  buffs.the_rotten->set_default_value_from_effect( 1 ); // Combo Point Gain

  buffs.guile_charm_insight_1 = make_buff( this, "shallow_insight", find_spell( 340582 ) )
    ->set_default_value_from_effect( 1 ) // Bonus Damage%
    ->add_invalidate( CACHE_PLAYER_DAMAGE_MULTIPLIER )
    ->set_stack_change_callback( [ this ]( buff_t*, int, int ) {
      legendary.guile_charm_counter = 0;
    } );
  buffs.guile_charm_insight_2 = make_buff( this, "moderate_insight", find_spell( 340583 ) )
    ->set_default_value_from_effect( 1 ) // Bonus Damage%
    ->add_invalidate( CACHE_PLAYER_DAMAGE_MULTIPLIER )
    ->set_stack_change_callback( [ this ]( buff_t*, int, int ) {
      buffs.guile_charm_insight_1->expire();
      legendary.guile_charm_counter = 0;
    } );
  buffs.guile_charm_insight_3 = make_buff( this, "deep_insight", find_spell( 340584 ) )
    ->set_default_value_from_effect( 1 ) // Bonus Damage%
    ->add_invalidate( CACHE_PLAYER_DAMAGE_MULTIPLIER )
    ->set_stack_change_callback( [ this ]( buff_t*, int, int ) {
      buffs.guile_charm_insight_2->expire();
      legendary.guile_charm_counter = 0;
    } );

  if ( talent.subtlety.finality->ok() )
  {
    // Talent ranks override the value and duration of the buffs via dummy effects
    timespan_t duration = timespan_t::from_seconds( talent.subtlety.finality->effectN( 2 ).base_value() );
    buffs.finality_black_powder = make_buff<damage_buff_t>( this, "finality_black_powder", spec.finality_black_powder_buff,
                                                            talent.subtlety.finality->effectN( 1 ).percent() );
    buffs.finality_black_powder->set_duration( duration );
    buffs.finality_eviscerate = make_buff<damage_buff_t>( this, "finality_eviscerate", spec.finality_eviscerate_buff,
                                                          talent.subtlety.finality->effectN( 1 ).percent() );
    buffs.finality_eviscerate->set_duration( duration );
    buffs.finality_rupture = make_buff( this, "finality_rupture", spec.finality_rupture_buff )
      ->set_default_value( talent.subtlety.finality->effectN( 1 ).percent() )
      ->set_duration( duration );
  }
  else
  {
    buffs.finality_black_powder = make_buff<damage_buff_t>( this, "finality_black_powder", spec.finality_black_powder_buff );
    buffs.finality_eviscerate = make_buff<damage_buff_t>( this, "finality_eviscerate", spec.finality_eviscerate_buff );
    buffs.finality_rupture = make_buff( this, "finality_rupture", spec.finality_rupture_buff )
      ->set_default_value_from_effect( 1 ); // Bonus Damage%
  }

  buffs.concealed_blunderbuss = make_buff( this, "concealed_blunderbuss", find_spell( 340587 ) )
    ->set_chance( talent.outlaw.fan_the_hammer->ok() ? 0.0 : legendary.concealed_blunderbuss->effectN( 1 ).percent() )
    ->set_default_value( legendary.concealed_blunderbuss->effectN( 2 ).base_value() );

  buffs.greenskins_wickers = make_buff( this, "greenskins_wickers", spec.greenskins_wickers_buff )
    ->set_default_value_from_effect_type( A_ADD_PCT_MODIFIER, P_GENERIC );
}

// rogue_t::create_options ==================================================

static bool do_parse_secondary_weapon( rogue_t* rogue, util::string_view value, slot_e slot )
{
  switch ( slot )
  {
    case SLOT_MAIN_HAND:
      rogue -> weapon_data[ WEAPON_MAIN_HAND ].secondary_weapon_data = item_t( rogue, value );
      rogue -> weapon_data[ WEAPON_MAIN_HAND ].secondary_weapon_data.slot = slot;
      break;
    case SLOT_OFF_HAND:
      rogue -> weapon_data[ WEAPON_OFF_HAND ].secondary_weapon_data = item_t( rogue, value );
      rogue -> weapon_data[ WEAPON_OFF_HAND ].secondary_weapon_data.slot = slot;
      break;
    default:
      break;
  }

  return true;
}

static bool parse_offhand_secondary( sim_t* sim, util::string_view /* name */, util::string_view value )
{
  rogue_t* rogue = static_cast<rogue_t*>( sim -> active_player );
  return do_parse_secondary_weapon( rogue, value, SLOT_OFF_HAND );
}

static bool parse_mainhand_secondary( sim_t* sim, util::string_view /* name */, util::string_view value )
{
  rogue_t* rogue = static_cast<rogue_t*>( sim -> active_player );
  return do_parse_secondary_weapon( rogue, value, SLOT_MAIN_HAND );
}

static bool parse_fixed_rtb( sim_t* sim, util::string_view /* name */, util::string_view value )
{
  std::vector<size_t> buffs;
  for ( auto it = value.begin(); it < value.end(); ++it )
  {
    if ( *it < '1' or *it > '6' )
    {
      continue;
    }

    size_t buff_index = *it - '1';
    if ( range::find( buffs, buff_index ) != buffs.end() )
    {
      sim->errorf( "%s: Duplicate 'fixed_rtb' buff %c", sim->active_player->name(), *it );
      return false;
    }

    buffs.push_back( buff_index );
  }

  if ( buffs.empty() || buffs.size() > 6 )
  {
    sim->error( "{}: No valid 'fixed_rtb' buffs given by string '{}'", sim->active_player->name(), value );
    return false;
  }

  debug_cast<rogue_t*>( sim->active_player )->options.fixed_rtb = buffs;

  return true;
}

static bool parse_fixed_rtb_odds( sim_t* sim, util::string_view /* name */, util::string_view value )
{
  auto odds = util::string_split<util::string_view>( value, "," );
  if ( odds.size() != 6 )
  {
    sim->errorf( "%s: Expected 6 comma-separated values for 'fixed_rtb_odds'", sim->active_player->name() );
    return false;
  }

  std::vector<double> buff_chances;
  buff_chances.resize( 6, 0.0 );
  double sum = 0.0;
  for ( size_t i = 0; i < odds.size(); i++ )
  {
    buff_chances[ i ] = util::to_double( odds[ i ] );
    sum += buff_chances[ i ];
  }

  if ( sum != 100.0 )
  {
    sim->errorf( "Warning: %s: 'fixed_rtb_odds' adding up to %f instead of 100, re-scaling accordingly", sim->active_player->name(), sum );
    for ( size_t i = 0; i < odds.size(); i++ )
    {
      buff_chances[ i ] = buff_chances[ i ] / sum * 100.0;
    }
  }

  debug_cast<rogue_t*>( sim->active_player )->options.fixed_rtb_odds = buff_chances;
  return true;
}

void rogue_t::create_options()
{
  player_t::create_options();

  // Overload default options but with a default true value
  add_option( opt_bool( "ready_trigger", options.rogue_ready_trigger ) );

  add_option( opt_func( "off_hand_secondary", parse_offhand_secondary ) );
  add_option( opt_func( "main_hand_secondary", parse_mainhand_secondary ) );
  add_option( opt_int( "initial_combo_points", options.initial_combo_points ) );
  add_option( opt_int( "initial_shadow_techniques", options.initial_shadow_techniques, -1, 4 ) );
  add_option( opt_func( "fixed_rtb", parse_fixed_rtb ) );
  add_option( opt_func( "fixed_rtb_odds", parse_fixed_rtb_odds ) );
  add_option( opt_bool( "prepull_shadowdust", options.prepull_shadowdust ) );
  add_option( opt_bool( "priority_rotation", options.priority_rotation ) );
}

// rogue_t::copy_from =======================================================

void rogue_t::copy_from( player_t* source )
{
  rogue_t* rogue = static_cast<rogue_t*>( source );
  player_t::copy_from( source );

  if ( !rogue->weapon_data[ WEAPON_MAIN_HAND ].secondary_weapon_data.options_str.empty() )
  {
    weapon_data[ WEAPON_MAIN_HAND ].secondary_weapon_data.options_str = \
      rogue->weapon_data[ WEAPON_MAIN_HAND ].secondary_weapon_data.options_str;
  }

  if ( !rogue->weapon_data[ WEAPON_OFF_HAND ].secondary_weapon_data.options_str.empty() )
  {
    weapon_data[ WEAPON_OFF_HAND ].secondary_weapon_data.options_str = \
      rogue->weapon_data[ WEAPON_OFF_HAND ].secondary_weapon_data.options_str;
  }

  options.initial_combo_points = rogue->options.initial_combo_points;
  options.initial_shadow_techniques = rogue->options.initial_shadow_techniques;

  options.fixed_rtb = rogue->options.fixed_rtb;
  options.fixed_rtb_odds = rogue->options.fixed_rtb_odds;
  options.rogue_ready_trigger = rogue->options.rogue_ready_trigger;
  options.prepull_shadowdust = rogue->options.prepull_shadowdust;
  options.priority_rotation = rogue->options.priority_rotation;
}

// rogue_t::create_profile  =================================================

std::string rogue_t::create_profile( save_e stype )
{
  std::string profile_str = player_t::create_profile( stype );

  // Break out early if we are not saving everything, or gear
  if ( !(stype & SAVE_PLAYER ) && !(stype & SAVE_GEAR ) )
  {
    return profile_str;
  }

  std::string term = "\n";

  if ( weapon_data[ WEAPON_MAIN_HAND ].secondary_weapon_data.active() ||
       weapon_data[ WEAPON_OFF_HAND ].secondary_weapon_data.active() )
  {
    profile_str += term;
    profile_str += "# Secondary weapons used in conjunction with weapon swapping are defined below.";
    profile_str += term;
  }

  if ( weapon_data[ WEAPON_MAIN_HAND ].secondary_weapon_data.active() )
  {
    profile_str += "main_hand_secondary=";
    profile_str += weapon_data[ WEAPON_MAIN_HAND ].secondary_weapon_data.encoded_item() + term;
    if ( sim -> save_gear_comments &&
         ! weapon_data[ WEAPON_MAIN_HAND ].secondary_weapon_data.encoded_comment().empty() )
    {
      profile_str += "# ";
      profile_str += weapon_data[ WEAPON_MAIN_HAND ].secondary_weapon_data.encoded_comment();
      profile_str += term;
    }
  }

  if ( weapon_data[ WEAPON_OFF_HAND ].secondary_weapon_data.active() )
  {
    profile_str += "off_hand_secondary=";
    profile_str += weapon_data[ WEAPON_OFF_HAND ].secondary_weapon_data.encoded_item() + term;
    if ( sim -> save_gear_comments &&
         ! weapon_data[ WEAPON_OFF_HAND ].secondary_weapon_data.encoded_comment().empty() )
    {
      profile_str += "# ";
      profile_str += weapon_data[ WEAPON_OFF_HAND ].secondary_weapon_data.encoded_comment();
      profile_str += term;
    }
  }

  return profile_str;
}

// rogue_t::init_items ======================================================

void rogue_t::init_items()
{
  player_t::init_items();

  // Initialize weapon swapping data structures for primary weapons here
  weapon_data[ WEAPON_MAIN_HAND ].weapon_data[ WEAPON_PRIMARY ] = main_hand_weapon;
  weapon_data[ WEAPON_MAIN_HAND ].item_data[ WEAPON_PRIMARY ] = &( items[ SLOT_MAIN_HAND ] );
  weapon_data[ WEAPON_OFF_HAND ].weapon_data[ WEAPON_PRIMARY ] = off_hand_weapon;
  weapon_data[ WEAPON_OFF_HAND ].item_data[ WEAPON_PRIMARY ] = &( items[ SLOT_OFF_HAND ] );

  if ( ! weapon_data[ WEAPON_MAIN_HAND ].secondary_weapon_data.options_str.empty() )
  {
    weapon_data[ WEAPON_MAIN_HAND ].secondary_weapon_data.init();
    weapon_data[ WEAPON_MAIN_HAND ].weapon_data[ WEAPON_SECONDARY ] = main_hand_weapon;
    weapon_data[ WEAPON_MAIN_HAND ].item_data[ WEAPON_SECONDARY ] = &( weapon_data[ WEAPON_MAIN_HAND ].secondary_weapon_data );

    // Restore primary main hand weapon after secondary weapon init
    main_hand_weapon = weapon_data[ WEAPON_MAIN_HAND ].weapon_data[ WEAPON_PRIMARY ];
  }

  if ( ! weapon_data[ WEAPON_OFF_HAND ].secondary_weapon_data.options_str.empty() )
  {
    weapon_data[ WEAPON_OFF_HAND ].secondary_weapon_data.init();
    weapon_data[ WEAPON_OFF_HAND ].weapon_data[ WEAPON_SECONDARY ] = off_hand_weapon;
    weapon_data[ WEAPON_OFF_HAND ].item_data[ WEAPON_SECONDARY ] = &( weapon_data[ WEAPON_OFF_HAND ].secondary_weapon_data );

    // Restore primary off hand weapon after secondary weapon init
    main_hand_weapon = weapon_data[ WEAPON_OFF_HAND ].weapon_data[ WEAPON_PRIMARY ];
  }
}

// rogue_t::init_special_effects ============================================

// Sylvanas Dagger
struct banshees_blight_t : public unique_gear::scoped_actor_callback_t<rogue_t>
{
  // Debuff trigger spell from the driver 357595
  // This has a 0.5s ICD and triggers from both yellow and white direct attacks
  // Triggers the debuff 358090 which applies stacks based on remaining health
  // If the target heals for any reason, the stack does not regress (as seen on dummies)
  struct banshees_blight_debuff_t : public unique_gear::proc_spell_t
  {
    rogue_t* rogue;

    banshees_blight_debuff_t( const special_effect_t& e )
      : proc_spell_t( "banshees_blight_debuff", e.player, e.driver(), e.item ),
      rogue( debug_cast<rogue_t*>( e.player ) )
    {
      quiet = dual = true;
    }

    void impact( action_state_t* s ) override
    {
      rogue_td_t* td = rogue->get_target_data( s->target );
      double health_percentage = s->target->health_percentage();
      int current_stacks = td->debuffs.banshees_blight->check();
      int desired_stacks = health_percentage < 25 ? 4 :
                           health_percentage < 50 ? 3 :
                           health_percentage < 75 ? 2 : 1;

      if ( current_stacks < desired_stacks )
      {
        td->debuffs.banshees_blight->trigger( desired_stacks - current_stacks );
      }
    }
  };

  // Damage trigger spell from 358126
  // Damage values are stored on both the item driver 357595 and hotfixed spell 359180
  // This attack triggers multiple times based on target stack count, handled in trigger_banshees_blight
  // When dual-wielding, the item damage amounts from both weapons are added together
  struct banshees_blight_damage_t : public unique_gear::proc_spell_t
  {
    double scaled_dmg;

    banshees_blight_damage_t( const special_effect_t& e )
      : proc_spell_t( "banshees_blight", e.player, e.player->find_spell( 358126 ), e.item ),
      scaled_dmg( e.driver()->effectN( 1 ).average( e.item ) )
    {
      base_dd_min = base_dd_max = scaled_dmg;
    }

    double base_da_min( const action_state_t* ) const override
    { return scaled_dmg; }

    double base_da_max( const action_state_t* ) const override
    { return scaled_dmg; }
  };

  banshees_blight_t() : super( ROGUE )
  {}

  void initialize( special_effect_t& e ) override
  {
    unique_gear::scoped_actor_callback_t<rogue_t>::initialize( e );

    // Create callback action to proc debuff stacks on the target
    // Only create one instance of this, as dual-wielding does not change the application of the debuff
    if ( e.player->find_action( "banshees_blight_debuff" ) )
      return;

    e.execute_action = unique_gear::create_proc_action<banshees_blight_debuff_t>( "banshees_blight_debuff", e );
    new dbc_proc_callback_t( e.player, e );
  }

  void manipulate( rogue_t* rogue, const special_effect_t& e ) override
  {
    rogue->legendary.banshees_blight = e.driver();

    // Create damage action triggered by actions::rogue_action_t<Base>::trigger_banshees_blight
    auto banshees_blight_damage = static_cast<banshees_blight_damage_t*>( e.player->find_action( "banshees_blight" ) );
    if ( !banshees_blight_damage )
      rogue->active.banshees_blight = unique_gear::create_proc_action<banshees_blight_damage_t>( "banshees_blight", e );
    else
      banshees_blight_damage->scaled_dmg += e.driver()->effectN( 1 ).average( e.item );
  }
};

void rogue_t::init_special_effects()
{
  player_t::init_special_effects();

  if ( weapon_data[ WEAPON_MAIN_HAND ].item_data[ WEAPON_SECONDARY ] )
  {
    for ( size_t i = 0, end = weapon_data[ WEAPON_MAIN_HAND ].item_data[ WEAPON_SECONDARY ] -> parsed.special_effects.size();
          i < end; ++i )
    {
      special_effect_t* effect = weapon_data[ WEAPON_MAIN_HAND ].item_data[ WEAPON_SECONDARY ] -> parsed.special_effects[ i ];
      unique_gear::initialize_special_effect_2( effect );
    }
  }

  if ( weapon_data[ WEAPON_OFF_HAND ].item_data[ WEAPON_SECONDARY ] )
  {
    for ( size_t i = 0, end = weapon_data[ WEAPON_OFF_HAND ].item_data[ WEAPON_SECONDARY ] -> parsed.special_effects.size();
          i < end; ++i )
    {
      special_effect_t* effect = weapon_data[ WEAPON_OFF_HAND ].item_data[ WEAPON_SECONDARY ] -> parsed.special_effects[ i ];
      unique_gear::initialize_special_effect_2( effect );
    }
  }
}

// rogue_t::init_finished ===================================================

void rogue_t::init_finished()
{
  weapon_data[ WEAPON_MAIN_HAND ].initialize();
  weapon_data[ WEAPON_OFF_HAND ].initialize();

  player_t::init_finished();
}

// rogue_t::reset ===========================================================

void rogue_t::reset()
{
  player_t::reset();

  if( options.initial_shadow_techniques >= 0 )
    shadow_techniques_counter = options.initial_shadow_techniques;
  else
    shadow_techniques_counter = rng().range( 0, 5 );

  danse_macabre_tracker.clear();

  restealth_allowed = false;

  weapon_data[ WEAPON_MAIN_HAND ].reset();
  weapon_data[ WEAPON_OFF_HAND ].reset();
}

// rogue_t::activate ========================================================

struct restealth_callback_t
{
  rogue_t* r;
  restealth_callback_t( rogue_t* p ) : r( p )
  { }

  void operator()( player_t* )
  {
    // Special case: We allow stealth when the actor has no active targets
    // (which excludes invulnerable ones if ignore_invulnerable_targets is set like in the DungeonSlice fightstyle).
    // This allows us to better approximate restealthing in dungeons.
    if ( r->sim->target_non_sleeping_list.empty() )
    {
      r->restealth_allowed = true;
      r->cancel_auto_attacks();
    }
  }
};

void rogue_t::activate()
{
  player_t::activate();

  sim->target_non_sleeping_list.register_callback( restealth_callback_t( this ) );
}

// rogue_t::break_stealth ===================================================

void rogue_t::break_stealth()
{
  // Trigger Subterfuge
  if ( talent.rogue.subterfuge->ok() && !buffs.subterfuge->check() && stealthed( STEALTH_BASIC ) )
  {
    buffs.subterfuge->trigger();
  }

  // DFALPHA -- Trigger Improved Garrote

  // Expiry delayed by 1ms in order to have it processed on the next tick. This seems to be what the server does.
  if ( player_t::buffs.shadowmeld->check() )
  {
    player_t::buffs.shadowmeld->expire( 1_ms );
  }

  if ( buffs.stealth->check() )
  {
    buffs.stealth->expire( 1_ms );
  }

  if ( buffs.vanish->check() )
  {
    buffs.vanish->expire( 1_ms );
  }
}

// rogue_t::cancel_auto_attack ==============================================

void rogue_t::cancel_auto_attacks()
{
  // Cancel scheduled AA events and record the swing timer to reference on restart
  if ( melee_main_hand && melee_main_hand->execute_event )
  {
    melee_main_hand->canceled = true;
    melee_main_hand->prev_scheduled_time = melee_main_hand->execute_event->occurs();
  }

  if ( melee_off_hand && melee_off_hand->execute_event )
  {
    melee_off_hand->canceled = true;
    melee_off_hand->prev_scheduled_time = melee_off_hand->execute_event->occurs();
  }

  player_t::cancel_auto_attacks();
}

// rogue_t::swap_weapon =====================================================

void rogue_t::swap_weapon( weapon_slot_e slot, current_weapon_e to_weapon, bool in_combat )
{
  if ( weapon_data[ slot ].current_weapon == to_weapon )
  {
    return;
  }

  if ( ! weapon_data[ slot ].item_data[ to_weapon ] )
  {
    return;
  }

  sim->print_debug( "{} performing weapon swap from {} to {}", *this,
                    *weapon_data[ slot ].item_data[ !to_weapon ], *weapon_data[ slot ].item_data[ to_weapon ] );

  // First, swap stats on actor, but only if it is in combat. Outside of combat (basically
  // during iteration reset) there is no need to adjust actor stats, as they are always reset to
  // the primary weapons.
  for ( stat_e i = STAT_NONE; in_combat && i < STAT_MAX; i++ )
  {
    stat_loss( i, weapon_data[ slot ].stats_data[ ! to_weapon ].get_stat( i ) );
    stat_gain( i, weapon_data[ slot ].stats_data[ to_weapon ].get_stat( i ) );
  }

  weapon_t* target_weapon = &( slot == WEAPON_MAIN_HAND ? main_hand_weapon : off_hand_weapon );
  action_t* target_action = slot == WEAPON_MAIN_HAND ? main_hand_attack : off_hand_attack;

  // Swap the actor weapon object
  *target_weapon = weapon_data[ slot ].weapon_data[ to_weapon ];
  target_action -> base_execute_time = target_weapon -> swing_time;

  // Enable new weapon callback(s)
  weapon_data[ slot ].callback_state( to_weapon, true );

  // Disable old weapon callback(s)
  weapon_data[ slot ].callback_state( static_cast<current_weapon_e>( ! to_weapon ), false );

  // Reset swing timer of the weapon
  if ( target_action -> execute_event )
  {
    event_t::cancel( target_action -> execute_event );
    target_action -> schedule_execute();
  }

  // Track uptime
  if ( to_weapon == WEAPON_PRIMARY )
  {
    weapon_data[ slot ].secondary_weapon_uptime -> expire();
  }
  else
  {
    weapon_data[ slot ].secondary_weapon_uptime -> trigger();
  }

  // Set the current weapon wielding state for the slot
  weapon_data[ slot ].current_weapon = to_weapon;
}

// rogue_t::stealthed =======================================================

bool rogue_t::stealthed( uint32_t stealth_mask ) const
{
  if ( ( stealth_mask & STEALTH_NORMAL ) && buffs.stealth->check() )
    return true;

  if ( ( stealth_mask & STEALTH_VANISH ) && buffs.vanish->check() )
    return true;

  if ( ( stealth_mask & STEALTH_SHADOWDANCE ) && buffs.shadow_dance->check() )
    return true;

  if ( ( stealth_mask & STEALTH_SUBTERFUGE ) && buffs.subterfuge->check() )
    return true;

  if ( ( stealth_mask & STEALTH_SHADOWMELD ) && player_t::buffs.shadowmeld->check() )
    return true;

  if ( ( stealth_mask & STEALTH_SEPSIS ) && buffs.sepsis->check() )
    return true;

  if ( ( stealth_mask & STEALTH_IMPROVED_GARROTE ) &&
       ( buffs.improved_garrote->check() || buffs.improved_garrote_aura->check() ) )
    return true;

  return false;
}

// rogue_t::arise ===========================================================

void rogue_t::arise()
{
  player_t::arise();

  resources.current[ RESOURCE_COMBO_POINT ] = 0;
}

// rogue_t::combat_begin ====================================================

void rogue_t::combat_begin()
{
  player_t::combat_begin();
}

// rogue_t::energy_regen_per_second =========================================

double rogue_t::resource_regen_per_second( resource_e r ) const
{
  double reg = player_t::resource_regen_per_second( r );

  // We handle some energy gain increases in rogue_t::regen() instead of the resource_regen_per_second method in order to better track their benefits.
  // For simple implementation without separate tracking, can put stuff here.

  return reg;
}

// rogue_t::temporary_movement_modifier ==================================

double rogue_t::temporary_movement_modifier() const
{
  double temporary = player_t::temporary_movement_modifier();

  if ( buffs.sprint->up() )
    temporary = std::max( buffs.sprint->data().effectN( 1 ).percent(), temporary );

  if ( buffs.shadowstep->up() )
    temporary = std::max( buffs.shadowstep->data().effectN( 2 ).percent(), temporary );

  return temporary;
}

// rogue_t::passive_movement_modifier===================================

double rogue_t::passive_movement_modifier() const
{
  double ms = player_t::passive_movement_modifier();

  ms += talent.rogue.fleet_footed->effectN( 1 ).percent();
  ms += spell.fleet_footed->effectN( 1 ).percent(); // DFALPHA: Duplicate passive?
  ms += talent.outlaw.hit_and_run->effectN( 1 ).percent();

  if ( stealthed( ( STEALTH_BASIC | STEALTH_SHADOWDANCE ) ) )
  {
    ms += talent.rogue.shadowrunner->effectN( 1 ).percent();
  }

  return ms;
}

// rogue_t::regen ===========================================================

void rogue_t::regen( timespan_t periodicity )
{
  player_t::regen( periodicity );

  // We handle some energy gain increases here instead of the resource_regen_per_second method in order to better track their benefits.
  // IMPORTANT NOTE: If anything is updated/added here, rogue_t::create_resource_expression() needs to be updated as well to reflect this
  if ( ! resources.is_infinite( RESOURCE_ENERGY ) )
  {
    // Multiplicative energy gains
    double mult_regen_base = periodicity.total_seconds() * resource_regen_per_second( RESOURCE_ENERGY );

    if ( buffs.adrenaline_rush->up() )
    {
      double energy_regen = mult_regen_base * buffs.adrenaline_rush->data().effectN( 1 ).percent();
      resource_gain( RESOURCE_ENERGY, energy_regen, gains.adrenaline_rush );
      mult_regen_base += energy_regen;
    }

    // Additive energy gains
    if ( buffs.buried_treasure->up() )
      resource_gain( RESOURCE_ENERGY, buffs.buried_treasure -> check_value() * periodicity.total_seconds(), gains.buried_treasure );
  }
}

// rogue_t::available =======================================================

timespan_t rogue_t::available() const
{
  if ( ready_type != READY_POLL )
  {
    return player_t::available();
  }
  else
  {
    double energy = resources.current[ RESOURCE_ENERGY ];

    if ( energy > 25 )
      return timespan_t::from_seconds( 0.1 );

    return std::max(
             timespan_t::from_seconds( ( 25 - energy ) / resource_regen_per_second( RESOURCE_ENERGY ) ),
             timespan_t::from_seconds( 0.1 )
           );
  }
}

// rogue_t::convert_hybrid_stat ==============================================

stat_e rogue_t::convert_hybrid_stat( stat_e s ) const
{
  // this converts hybrid stats that either morph based on spec or only work
  // for certain specs into the appropriate "basic" stats
  switch ( s )
  {
  case STAT_STR_AGI_INT:
  case STAT_AGI_INT:
  case STAT_STR_AGI:
    return STAT_AGILITY;
  // This is a guess at how STR/INT gear will work for Rogues, TODO: confirm
  // This should probably never come up since rogues can't equip plate, but....
  case STAT_STR_INT:
    return STAT_NONE;
  case STAT_SPIRIT:
      return STAT_NONE;
  case STAT_BONUS_ARMOR:
      return STAT_NONE;
  default: return s;
  }
}

void rogue_t::apply_affecting_auras( action_t& action )
{
  player_t::apply_affecting_auras( action );

  action.apply_affecting_aura( spell.all_rogue );
  action.apply_affecting_aura( spec.assassination_rogue );
  action.apply_affecting_aura( spec.outlaw_rogue );
  action.apply_affecting_aura( spec.subtlety_rogue );
}

// ROGUE MODULE INTERFACE ===================================================

class rogue_module_t : public module_t
{
public:
  rogue_module_t() : module_t( ROGUE ) {}

  player_t* create_player( sim_t* sim, util::string_view name, race_e r = RACE_NONE ) const override
  {
    return new rogue_t( sim, name, r );
  }

  bool valid() const override
  { return true; }

  void static_init() const override
  {
    unique_gear::register_special_effect( 357595, banshees_blight_t() ); // Sylvanas Dagger
  }

  void register_hotfixes() const override
  {
    // Only need to change the base effect and not also 894305 since we re-use the base effect for the persist
    // Have to do this manually since it appears this is currently being done with server-side voodoo
    hotfix::register_effect( "Rogue", "2021-07-08", "Obedience: Versatility granted reduced to 0.5% per stack of Flagellation (was 1% per stack).", 887338 )
      .field( "base_value" )
      .operation( hotfix::HOTFIX_SET )
      .modifier( 0.5 )
      .verification_value( 1 );
  }

  void init( player_t* ) const override {}
  void combat_begin( sim_t* ) const override {}
  void combat_end( sim_t* ) const override {}
};

} // UNNAMED NAMESPACE

const module_t* module_t::rogue()
{
  static rogue_module_t m;
  return &m;
}
