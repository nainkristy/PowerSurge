import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PIN

# Define the namespace and class
bl0937_fast_ns = cg.esphome_ns.namespace("bl0937_fast")
BL0937Fast = bl0937_fast_ns.class_("BL0937Fast", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(BL0937Fast),
    cv.Required(CONF_PIN): cv.int_,
    cv.Required("multiplier"): cv.float_,
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID], config[CONF_PIN], config["multiplier"])
    await cg.register_component(var, config)
