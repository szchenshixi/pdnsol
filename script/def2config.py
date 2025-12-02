#!/usr/bin/env python3
"""
DEF to C++ PDN Configuration Generator - Full Extraction
Extracts ALL metal layers and vias exactly as defined in DEF files
"""

import re
import sys
import argparse
from typing import Dict, List, Set, Tuple
import json


class DEFExtractor:
    """Extracts complete metal layer and via information from DEF files."""
    def __init__(self, def_file: str):
        self.def_file = def_file
        self.vias = {}  # via_name -> {layers: [bottom, via, top], ...}
        self.metal_layers = set()  # All metal layers found
        self.power_nets = set()
        self.ground_nets = set()
        self.all_layers_from_def = set()  # All layer names mentioned anywhere

    def parse(self) -> bool:
        """Parse DEF file and extract all layer/via definitions."""
        try:
            with open(self.def_file, 'r') as f:
                content = f.read()
        except FileNotFoundError:
            print(f"Error: File {self.def_file} not found")
            return False

        # Extract ALL layer names from the file (for reference)
        self._extract_all_layer_names(content)

        # Parse VIAS section for exact via definitions
        self._parse_vias_section(content)

        # Parse SPECIALNETS for power/ground nets and additional layer usage
        self._parse_specialnets_section(content)

        # Extract metal layers from via definitions
        self._extract_metal_layers_from_vias()

        return True

    def _extract_all_layer_names(self, content: str):
        """Extract all layer names mentioned in DEF (for debugging)."""
        # Look for any word that looks like a layer name
        layer_pattern = r'\b(met|metal|M|poly|diff|nwell|pwell|contact|via|via\d*|m|metal)\d*\b'
        all_matches = re.findall(layer_pattern, content, re.IGNORECASE)
        self.all_layers_from_def = set(all_matches)

    def _parse_vias_section(self, content: str):
        """Parse VIAS section to get exact via definitions."""
        # Find VIAS section
        vias_pattern = r'VIAS\s+\d+\s*;(.*?)END\s+VIAS'
        vias_match = re.search(vias_pattern, content,
                               re.IGNORECASE | re.DOTALL)

        if not vias_match:
            print("Warning: No VIAS section found")
            return

        vias_section = vias_match.group(1)

        # Split by via definitions (each via ends with ;)
        via_defs = re.split(r';\s*(?=-)', vias_section.strip())

        for via_def in via_defs:
            if not via_def.strip() or via_def == ';':
                continue

            # Extract via name (first token after -)
            via_match = re.match(r'-\s*(\S+)', via_def)
            if not via_match:
                continue

            via_name = via_match.group(1)
            via_info = {'name': via_name}

            # Extract LAYERS information
            layers_match = re.search(r'LAYERS\s+(\S+)\s+(\S+)\s+(\S+)',
                                     via_def)
            if layers_match:
                bottom_layer, via_layer, top_layer = layers_match.groups()
                via_info['layers'] = [bottom_layer, via_layer, top_layer]
                via_info['bottom_layer'] = bottom_layer
                via_info['top_layer'] = top_layer

            # Extract ROWCOL information
            rowcol_match = re.search(r'ROWCOL\s+(\d+)\s+(\d+)', via_def)
            if rowcol_match:
                via_info['rows'] = int(rowcol_match.group(1))
                via_info['cols'] = int(rowcol_match.group(2))

            # Extract CUTSIZE information
            cutsize_match = re.search(r'CUTSIZE\s+(\d+)\s+(\d+)', via_def)
            if cutsize_match:
                via_info['cut_width'] = int(cutsize_match.group(1))
                via_info['cut_height'] = int(cutsize_match.group(2))

            self.vias[via_name] = via_info

    def _parse_specialnets_section(self, content: str):
        """Parse SPECIALNETS section for power/ground nets."""
        specialnets_pattern = r'SPECIALNETS\s+\d+\s*;(.*?)END\s+SPECIALNETS'
        specialnets_match = re.search(specialnets_pattern, content,
                                      re.IGNORECASE | re.DOTALL)

        if not specialnets_match:
            return

        specialnets_section = specialnets_match.group(1)

        # Find all net definitions
        net_pattern = r'-(\w+)\s+\([^)]+\)\s+\+\s+USE\s+(POWER|GROUND|SIGNAL)'
        net_matches = re.findall(net_pattern, specialnets_section)

        for net_name, net_type in net_matches:
            if net_type.upper() == 'POWER':
                self.power_nets.add(net_name)
            elif net_type.upper() == 'GROUND':
                self.ground_nets.add(net_name)

    def _extract_metal_layers_from_vias(self):
        """Extract metal layers from via definitions."""
        for via_info in self.vias.values():
            if 'layers' in via_info:
                bottom_layer = via_info['layers'][0]
                top_layer = via_info['layers'][2]
                self.metal_layers.add(bottom_layer)
                self.metal_layers.add(top_layer)

    def get_layer_resistance_params(self,
                                    layer_name: str) -> Tuple[float, float]:
        """Get resistance parameters for a layer based on naming patterns."""
        # Extract layer number if possible
        layer_num_match = re.search(r'(\d+)', layer_name)
        layer_num = int(layer_num_match.group(1)) if layer_num_match else 1

        # Assign parameters based on layer number (higher layers typically thicker)
        if layer_num <= 3:  # Lower metal layers
            resistivity = 0.03  # Higher resistivity
            thickness = 0.2  # Thinner
        elif layer_num <= 6:  # Middle metal layers
            resistivity = 0.02
            thickness = 0.4
        else:  # Upper metal layers
            resistivity = 0.01  # Lower resistivity
            thickness = 0.8  # Thicker

        return resistivity, thickness

    def get_via_resistance(self, via_name: str, via_info: Dict) -> float:
        """Calculate via resistance based on dimensions."""
        # Default resistance values for different via types
        base_resistance = 0.001

        # Adjust based on dimensions if available
        if 'cut_width' in via_info and 'cut_height' in via_info:
            cut_area = via_info['cut_width'] * via_info['cut_height']
            # Larger area = lower resistance
            resistance = base_resistance * (1000 / cut_area)
        else:
            resistance = base_resistance

        # Adjust based on row/col if available
        if 'rows' in via_info and 'cols' in via_info:
            parallel_paths = via_info['rows'] * via_info['cols']
            resistance = resistance / parallel_paths

        return max(0.0001, min(resistance, 0.1))  # Clamp to reasonable range


class CppConfigGenerator:
    """Generate complete C++ configuration with ALL extracted definitions."""
    def __init__(self, extractor: DEFExtractor):
        self.extractor = extractor

    def generate_tech_db_config(self) -> List[str]:
        """Generate TechDatabase configuration section."""
        lines = []

        lines.append(
            '    // ============================================================'
        )
        lines.append('    // 1) Technology Database Setup')
        lines.append(
            '    // ============================================================'
        )
        lines.append('    TechDatabase techDb;')
        lines.append('')

        # Add ALL metal layers found
        if self.extractor.metal_layers:
            lines.append('    // Metal Layers - Add ALL layers from DEF')
            lines.append(
                '    // Format: addLayer(layer_name, resistivity_Ω·µm, thickness_µm)'
            )

            # Sort layers by extracting numbers if possible
            metal_layers_sorted = sorted(
                self.extractor.metal_layers,
                key=lambda x: int(re.search(r'(\d+)', x).group(1))
                if re.search(r'(\d+)', x) else 999)

            for layer in metal_layers_sorted:
                resistivity, thickness = self.extractor.get_layer_resistance_params(
                    layer)
                lines.append(
                    f'    techDb.addLayer("{layer}", {resistivity:.4f}, {thickness:.4f});'
                )
        else:
            lines.append('    // WARNING: No metal layers found in DEF file')

        lines.append('')

        # Add ALL vias from DEF
        if self.extractor.vias:
            lines.append('    // Vias - Add ALL vias from DEF VIAS section')
            lines.append(
                '    // Format: addVia(via_name, bottom_layer, top_layer, resistance_Ω)'
            )

            # Process vias in the order they appear in DEF
            for via_name, via_info in self.extractor.vias.items():
                if 'layers' in via_info and len(via_info['layers']) >= 3:
                    bottom_layer = via_info['layers'][0]
                    top_layer = via_info['layers'][2]
                    resistance = self.extractor.get_via_resistance(
                        via_name, via_info)

                    # Add dimensions in comment if available
                    dim_comment = ''
                    if 'cut_width' in via_info and 'cut_height' in via_info:
                        dim_comment = f' // {via_info["cut_width"]}x{via_info["cut_height"]}'

                    lines.append(
                        f'    techDb.addVia("{via_name}", "{bottom_layer}", "{top_layer}", {resistance:.6f});{dim_comment}'
                    )
                else:
                    lines.append(
                        f'    // WARNING: Via "{via_name}" has incomplete layer information'
                    )
        else:
            lines.append('    // WARNING: No vias found in DEF file')

        lines.append('')

        # Optional: Add custom via types if needed
        lines.append('    // Additional via types (if not in DEF)')
        lines.append(
            '    // techDb.addVia("custom_via", "met1", "met2", 0.001);')
        lines.append('')

        return lines

    def generate_pdn_config(self) -> List[str]:
        """Generate PDN configuration section."""
        lines = []

        lines.append(
            '    // ============================================================'
        )
        lines.append('    // 2) PDN Configuration')
        lines.append(
            '    // ============================================================'
        )
        lines.append('')

        # Power nets
        power_nets = list(self.extractor.power_nets)
        if not power_nets:
            power_nets = ['VDD']  # Default

        lines.append('    // Power nets from SPECIALNETS section')
        lines.append('    std::vector<std::string> powerNets = {')
        for i, net in enumerate(power_nets):
            comma = ',' if i < len(power_nets) - 1 else ''
            lines.append(f'        "{net}"{comma}')
        lines.append('    };')
        lines.append('')

        # Ground nets
        ground_nets = list(self.extractor.ground_nets)
        if not ground_nets:
            ground_nets = ['VSS']  # Default

        lines.append('    // Ground nets from SPECIALNETS section')
        lines.append('    std::vector<std::string> groundNets = {')
        for i, net in enumerate(ground_nets):
            comma = ',' if i < len(ground_nets) - 1 else ''
            lines.append(f'        "{net}"{comma}')
        lines.append('    };')
        lines.append('')

        # Layer order - use ALL metal layers found
        metal_layers_sorted = sorted(
            self.extractor.metal_layers,
            key=lambda x: int(re.search(r'(\d+)', x).group(1))
            if re.search(r'(\d+)', x) else 999)

        lines.append('    // Metal layer order (bottom to top)')
        lines.append('    // Includes ALL metal layers from via definitions')
        lines.append('    std::vector<std::string> layerOrder = {')
        for i, layer in enumerate(metal_layers_sorted):
            comma = ',' if i < len(metal_layers_sorted) - 1 else ''
            lines.append(f'        "{layer}"{comma}')
        lines.append('    };')
        lines.append('')

        return lines

    def generate_network_config(self) -> List[str]:
        """Generate network configuration section."""
        lines = []

        lines.append(
            '    // ============================================================'
        )
        lines.append('    // 3) Network Configuration (Optional)')
        lines.append(
            '    // ============================================================'
        )
        lines.append('')
        lines.append('    // Grid spacing (µm)')
        lines.append(
            '    double gridSpacing = 100.0;  // Adjust based on your design')
        lines.append('')
        lines.append('    // Bump/package pin locations (if known)')
        lines.append('    // std::vector<Point> bumpLocations = { ... };')
        lines.append('')
        lines.append('    // Current sources/sinks')
        lines.append('    // techDb.addCurrentSource("macroname", 1.0); // 1A')
        lines.append('')

        return lines

    def generate_full_config(self) -> str:
        """Generate complete C++ configuration."""
        lines = []

        # Header
        lines.append(
            '// ============================================================================'
        )
        lines.append('// PDN Configuration - Auto-generated from DEF')
        lines.append(
            '// ============================================================================'
        )
        lines.append(f'// Source: {self.extractor.def_file}')
        lines.append('//')
        lines.append(
            '// This configuration includes ALL metal layers and vias from the DEF file.'
        )
        lines.append(
            '// ============================================================================'
        )
        lines.append('')

        # Tech Database
        lines.extend(self.generate_tech_db_config())

        # PDN Configuration
        lines.extend(self.generate_pdn_config())

        # Network Configuration (Optional)
        lines.extend(self.generate_network_config())

        # Footer
        lines.append(
            '    // ============================================================'
        )
        lines.append('    // Notes:')
        lines.append(
            '    // 1. Verify all layers and vias are correctly extracted')
        lines.append(
            '    // 2. Adjust resistivity/thickness values for your process')
        lines.append('    // 3. Update layerOrder if needed')
        lines.append('    // 4. Add any missing via types not in DEF')
        lines.append(
            '    // ============================================================'
        )

        return '\n'.join(lines)

    def generate_summary(self) -> str:
        """Generate a summary of extracted information."""
        summary = []

        summary.append("=" * 80)
        summary.append("DEF EXTRACTION SUMMARY")
        summary.append("=" * 80)
        summary.append(f"File: {self.extractor.def_file}")
        summary.append("")

        # Metal layers
        summary.append("METAL LAYERS EXTRACTED:")
        summary.append("-" * 40)
        metal_layers_sorted = sorted(
            self.extractor.metal_layers,
            key=lambda x: int(re.search(r'(\d+)', x).group(1))
            if re.search(r'(\d+)', x) else 999)

        for layer in metal_layers_sorted:
            resistivity, thickness = self.extractor.get_layer_resistance_params(
                layer)
            summary.append(
                f"  {layer:10} -> Resistivity: {resistivity:.4f} Ω·µm, Thickness: {thickness:.4f} µm"
            )

        # Vias
        summary.append("")
        summary.append("VIAS EXTRACTED:")
        summary.append("-" * 40)
        for via_name, via_info in self.extractor.vias.items():
            if 'layers' in via_info and len(via_info['layers']) >= 3:
                bottom = via_info['layers'][0]
                top = via_info['layers'][2]
                resistance = self.extractor.get_via_resistance(
                    via_name, via_info)
                summary.append(
                    f"  {via_name:20} -> {bottom:6} to {top:6} (R = {resistance:.6f} Ω)"
                )

        # Power/Ground nets
        summary.append("")
        summary.append("POWER/GROUND NETS:")
        summary.append("-" * 40)
        if self.extractor.power_nets:
            summary.append(
                f"  Power nets:  {', '.join(self.extractor.power_nets)}")
        else:
            summary.append(
                "  No power nets found in SPECIALNETS (using default VDD)")

        if self.extractor.ground_nets:
            summary.append(
                f"  Ground nets: {', '.join(self.extractor.ground_nets)}")
        else:
            summary.append(
                "  No ground nets found in SPECIALNETS (using default VSS)")

        summary.append("")
        summary.append("=" * 80)

        return '\n'.join(summary)


def parse_command_line():
    """Parse command line arguments."""
    parser = argparse.ArgumentParser(
        description='Generate C++ PDN configuration from DEF files',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s design.def -o pdn_config.cpp
  %(prog)s design.def -o pdn_config.cpp -s
  %(prog)s design.def --json meta.json
        """)

    parser.add_argument('def_file', help='Input DEF file path')
    parser.add_argument('-o',
                        '--output',
                        default='pdn_config.cpp',
                        help='Output C++ file path (default: pdn_config.cpp)')
    parser.add_argument('-s',
                        '--summary',
                        action='store_true',
                        help='Print extraction summary')
    parser.add_argument('--json',
                        metavar='FILE',
                        help='Export extracted metadata as JSON')

    return parser.parse_args()


def export_to_json(extractor: DEFExtractor, json_file: str):
    """Export extracted data to JSON file."""
    data = {
        'source_file': extractor.def_file,
        'metal_layers': sorted(list(extractor.metal_layers)),
        'vias': extractor.vias,
        'power_nets': sorted(list(extractor.power_nets)),
        'ground_nets': sorted(list(extractor.ground_nets))
    }

    with open(json_file, 'w') as f:
        json.dump(data, f, indent=2)

    print(f"Metadata exported to: {json_file}")


def main():
    """Main function."""
    args = parse_command_line()

    # Extract information from DEF
    extractor = DEFExtractor(args.def_file)
    if not extractor.parse():
        sys.exit(1)

    # Generate summary if requested
    if args.summary:
        generator = CppConfigGenerator(extractor)
        print(generator.generate_summary())

    # Export to JSON if requested
    if args.json:
        export_to_json(extractor, args.json)

    # Generate C++ configuration
    generator = CppConfigGenerator(extractor)
    cpp_config = generator.generate_full_config()

    # Write to file
    with open(args.output, 'w') as f:
        f.write(cpp_config)

    print(f"\nC++ configuration generated: {args.output}")
    print(f"  - Metal layers: {len(extractor.metal_layers)}")
    print(f"  - Vias: {len(extractor.vias)}")
    print(
        f"  - Power nets: {len(extractor.power_nets) or 1} (default VDD if none)"
    )
    print(
        f"  - Ground nets: {len(extractor.ground_nets) or 1} (default VSS if none)"
    )


if __name__ == '__main__':
    main()
