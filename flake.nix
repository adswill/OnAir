{
  description = "OnAir: software radio receiver (DVB-T2, DVB-T, ATSC, FM and many more modes) for HackRF and other SDRs";

  # Built by the `nix` GitHub workflow; docs/NIXOS.md lists what is verified.
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f system nixpkgs.legacyPackages.${system});
      # SoapySDR modules put on the wrapper's plug-in path; names that this nixpkgs lacks are skipped
      soapyFor = pkgs: builtins.filter (p: p != null) (map (n: pkgs.${n} or null) [
        "soapyrtlsdr" "soapyhackrf" "soapyairspy" "soapyremote"
      ]);
      mkOnair = pkgs: server: pkgs.callPackage ./packaging/nix/package.nix {
        src = self;
        inherit server;
        soapyModules = if server then [ ] else soapyFor pkgs;
      };
    in
    {
      overlays.default = final: _prev: {
        onair = mkOnair final false;
        onair-server = mkOnair final true;
      };

      packages = forAll (system: pkgs: rec {
        onair = mkOnair pkgs false;
        onair-server = mkOnair pkgs true;
        default = onair;
      });

      apps = forAll (system: pkgs: rec {
        onair = { type = "app"; program = "${self.packages.${system}.onair}/bin/onair"; };
        onair-server = { type = "app"; program = "${self.packages.${system}.onair-server}/bin/onair-server"; };
        default = onair;
      });

      devShells = forAll (system: pkgs: {
        default = import ./packaging/nix/shell.nix { inherit pkgs; onair = self.packages.${system}.onair; };
      });

      nixosModules = rec {
        onair = import ./packaging/nix/module.nix { inherit self; };
        default = onair;
      };

      checks = forAll (system: pkgs:
        let
          # a NixOS configuration with the module: its evaluation catches option and flag mistakes without building a system
          eval = nixpkgs.lib.nixosSystem {
            inherit system;
            modules = [
              self.nixosModules.default
              {
                programs.onair.enable = true;
                services.onair-server = {
                  enable = true;
                  lan = true;
                  openFirewall = true;
                  web.enable = true;
                  web.lan = true;
                  extraArgs = [ "--mcp-stdio" ];
                };
                boot.loader.grub.enable = false;
                fileSystems."/" = { device = "none"; fsType = "tmpfs"; };
                system.stateVersion = "24.11";
              }
            ];
          };
          unit = eval.config.systemd.services.onair-server.serviceConfig.ExecStart;
        in
        {
          inherit (self.packages.${system}) onair onair-server;
          # the unit's command line, the firewall ports and the udev group, written to a file (store paths dropped: no package is built here)
          module = pkgs.writeText "onair-module-eval" (builtins.unsafeDiscardStringContext ''
            ${unit}
            ports: ${builtins.toString eval.config.networking.firewall.allowedTCPPorts}
            udev: ${builtins.toString (builtins.length eval.config.services.udev.packages)}
          '');
        });

      formatter = forAll (system: pkgs: pkgs.nixpkgs-fmt);
    };
}
