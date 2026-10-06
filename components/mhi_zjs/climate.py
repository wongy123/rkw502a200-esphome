import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import climate_ir, select

AUTO_LOAD = ["climate_ir", "select"]
CODEOWNERS = ["@angus"]

mhi_zjs_ns = cg.esphome_ns.namespace("mhi_zjs")
MhiZjsClimate = mhi_zjs_ns.class_("MhiZjsClimate", climate_ir.ClimateIR)

CONF_VERTICAL_SELECT = "vertical_position_select"
CONF_HORIZONTAL_SELECT = "horizontal_position_select"

CONFIG_SCHEMA = climate_ir.climate_ir_with_receiver_schema(MhiZjsClimate).extend(
    {
        cv.Optional(CONF_VERTICAL_SELECT): cv.use_id(select.Select),
        cv.Optional(CONF_HORIZONTAL_SELECT): cv.use_id(select.Select),
    }
)


async def to_code(config):
    var = await climate_ir.new_climate_ir(config)
    if vertical := config.get(CONF_VERTICAL_SELECT):
        cg.add(var.set_vertical_select(await cg.get_variable(vertical)))
    if horizontal := config.get(CONF_HORIZONTAL_SELECT):
        cg.add(var.set_horizontal_select(await cg.get_variable(horizontal)))
